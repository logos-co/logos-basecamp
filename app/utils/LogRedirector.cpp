#include "LogRedirector.h"

#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QSet>

#include <cerrno>
#include <cstdio>

#if !defined(Q_OS_WIN)
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace LogosBasecampLog {

LogRedirector& LogRedirector::instance()
{
    static LogRedirector s;
    return s;
}

LogRedirector::~LogRedirector()
{
    stop();
}

QString LogRedirector::filePath() const
{
    return m_filePath;
}

#if defined(Q_OS_WIN)

bool LogRedirector::start(const QString&, int, int) { return false; }
void LogRedirector::stop() {}
void LogRedirector::readerLoop() {}
void LogRedirector::openNewFile() {}
void LogRedirector::pruneOlderSessions() {}

#else

bool LogRedirector::start(const QString& logsDir, int maxLinesPerFile, int keepSessions)
{
    if (m_started)
        return true;
    if (maxLinesPerFile <= 0)
        maxLinesPerFile = 10000;

    m_logsDir = logsDir;
    m_maxLinesPerFile = maxLinesPerFile;
    m_keepSessions = keepSessions > 0 ? keepSessions : 1;

    if (!QDir().mkpath(m_logsDir))
        return false;

    m_sessionStamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_rotationIndex = 0;
    m_linesInCurrentFile = 0;

    // Before this session adds its own, so the directory settles at
    // keepSessions rather than one more.
    pruneOlderSessions();

    auto cleanup = [this]() {
        if (m_originalStdout >= 0) { ::close(m_originalStdout); m_originalStdout = -1; }
        if (m_originalStderr >= 0) { ::close(m_originalStderr); m_originalStderr = -1; }
        if (m_currentFile) {
            m_currentFile->close();
            m_currentFile.reset();
        }
        m_filePath.clear();
    };

    openNewFile();
    if (!m_currentFile || !m_currentFile->isOpen()) {
        cleanup();
        return false;
    }

    m_originalStdout = ::dup(fileno(stdout));
    m_originalStderr = ::dup(fileno(stderr));
    if (m_originalStdout < 0 || m_originalStderr < 0) {
        cleanup();
        return false;
    }

    int fds[2];
    if (::pipe(fds) != 0) {
        cleanup();
        return false;
    }

    std::fflush(stdout);
    std::fflush(stderr);

    if (::dup2(fds[1], fileno(stdout)) == -1 ||
        ::dup2(fds[1], fileno(stderr)) == -1) {
        ::close(fds[0]);
        ::close(fds[1]);
        cleanup();
        return false;
    }

    // Line-buffer so each line is forwarded through the pipe promptly, even
    // when stdout is not attached to a terminal.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    ::close(fds[1]);
    m_readFd = fds[0];

    m_running = true;
    m_readerThread = std::thread(&LogRedirector::readerLoop, this);
    m_started = true;
    return true;
}

void LogRedirector::stop()
{
    if (!m_started)
        return;
    m_running = false;

    std::fflush(stdout);
    std::fflush(stderr);

    // Restoring via dup2 closes the previous target fd, i.e. the pipe's
    // write end held by stdout/stderr. Once both are restored, the reader
    // thread sees EOF on the read end and exits. Keep the duplicated
    // originals open until after the reader thread has finished, since
    // readerLoop() may still be writing to m_originalStdout — closing it
    // concurrently could race with fd-number reuse.
    if (m_originalStdout >= 0)
        ::dup2(m_originalStdout, fileno(stdout));
    if (m_originalStderr >= 0)
        ::dup2(m_originalStderr, fileno(stderr));

    if (m_readerThread.joinable())
        m_readerThread.join();

    if (m_originalStdout >= 0) { ::close(m_originalStdout); m_originalStdout = -1; }
    if (m_originalStderr >= 0) { ::close(m_originalStderr); m_originalStderr = -1; }

    if (m_readFd >= 0) {
        ::close(m_readFd);
        m_readFd = -1;
    }
    if (m_currentFile) {
        m_currentFile->flush();
        m_currentFile->close();
        m_currentFile.reset();
    }
    m_started = false;
}

void LogRedirector::openNewFile()
{
    QString name;
    if (m_rotationIndex == 0) {
        name = QStringLiteral("basecamp_%1.log").arg(m_sessionStamp);
    } else {
        name = QStringLiteral("basecamp_%1.%2.log")
                   .arg(m_sessionStamp)
                   .arg(m_rotationIndex, 3, 10, QChar('0'));
    }

    auto f = std::make_unique<QFile>(m_logsDir + QStringLiteral("/") + name);
    if (!f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_currentFile.reset();
        return;
    }
    if (m_rotationIndex == 0)
        m_filePath = f->fileName();
    m_currentFile = std::move(f);
    m_linesInCurrentFile = 0;
    ++m_rotationIndex;
}

void LogRedirector::pruneOlderSessions()
{
    // A session is one stamp however many rotations it produced, so the newest
    // stamps are kept whole and everything older goes with its rotations.
    static const QRegularExpression sessionFile(
        QStringLiteral("^basecamp_(\\d{8}_\\d{6})(?:\\.\\d{3})?\\.log$"));

    const QStringList names = QDir(m_logsDir).entryList(QDir::Files, QDir::Name);
    QSet<QString> stamps;
    for (const QString& fileName : names) {
        const QRegularExpressionMatch match = sessionFile.match(fileName);
        if (match.hasMatch())
            stamps.insert(match.captured(1));
    }
    if (stamps.size() < m_keepSessions)
        return;

    QStringList ordered(stamps.begin(), stamps.end());
    ordered.sort();
    // Leaves room for the session about to start.
    const QStringList doomed = ordered.mid(0, ordered.size() - (m_keepSessions - 1));

    QDir dir(m_logsDir);
    for (const QString& fileName : names) {
        const QRegularExpressionMatch match = sessionFile.match(fileName);
        if (match.hasMatch() && doomed.contains(match.captured(1)))
            dir.remove(fileName);
    }
}

void LogRedirector::readerLoop()
{
    char buf[4096];
    while (true) {
        ssize_t n = ::read(m_readFd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break; // EOF: all writers (stdout/stderr replacements) are gone.

        // Mirror to the original stdout so an attached terminal keeps seeing
        // output. Failures here are ignored (e.g. stdout was /dev/null).
        if (m_originalStdout >= 0) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = ::write(m_originalStdout, buf + off, n - off);
                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                off += w;
            }
        }

        if (!m_currentFile)
            continue;

        ssize_t start = 0;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] != '\n')
                continue;
            m_currentFile->write(buf + start, i - start + 1);
            ++m_linesInCurrentFile;
            start = i + 1;
            if (m_linesInCurrentFile >= m_maxLinesPerFile) {
                m_currentFile->flush();
                m_currentFile->close();
                openNewFile();
                if (!m_currentFile)
                    return; // Rotation failed; stop writing to files.
            }
        }
        if (start < n && m_currentFile)
            m_currentFile->write(buf + start, n - start);
        // Per chunk, so anything tailing the file sees a line when it is
        // written rather than when the buffer fills or the session ends.
        if (m_currentFile)
            m_currentFile->flush();
    }

    if (m_currentFile)
        m_currentFile->flush();
}

#endif // Q_OS_WIN

} // namespace LogosBasecampLog
