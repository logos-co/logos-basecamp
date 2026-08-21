#include "LogRedirector.h"

#include "LogosBasecampPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMap>

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

int LogRedirector::pruneSessions(const QString& logsDir, int keep, qint64 maxBytes)
{
    const bool limitCount = keep >= 0;
    const bool limitSize = maxBytes > 0;
    if (!limitCount && !limitSize) return 0;

    // A session is one stamp however many rotations it produced: it stays
    // whole or goes whole. QMap keeps stamps sorted, i.e. oldest first.
    const QFileInfoList files = QDir(logsDir).entryInfoList(
        { QLatin1String(LogosBasecampPaths::kSessionLogGlob) },
        QDir::Files | QDir::NoSymLinks, QDir::Name);

    QMap<QString, QFileInfoList> byStamp;
    QMap<QString, qint64> bytesByStamp;
    qint64 total = 0;
    for (const QFileInfo& fi : files) {
        QString stamp;
        if (!LogosBasecampPaths::parseSessionLogFileName(fi.fileName(), &stamp, nullptr)) continue;
        byStamp[stamp].append(fi);
        bytesByStamp[stamp] += fi.size();
        total += fi.size();
    }

    int sessions = byStamp.size();
    int removed = 0;
    for (auto it = byStamp.cbegin(); it != byStamp.cend(); ++it) {
        const bool tooMany = limitCount && sessions > keep;
        const bool tooBig = limitSize && total > maxBytes;
        if (!tooMany && !tooBig) break;
        for (const QFileInfo& fi : it.value()) {
            if (QFile::remove(fi.absoluteFilePath())) ++removed;
        }
        total -= bytesByStamp.value(it.key());
        --sessions;
    }
    return removed;
}

#if defined(Q_OS_WIN)

bool LogRedirector::start(const QString&, int, int, qint64) { return false; }
void LogRedirector::stop() {}
void LogRedirector::readerLoop() {}
void LogRedirector::openNewFile() {}

#else

bool LogRedirector::start(const QString& logsDir, int maxLinesPerFile, int keepSessions,
                          qint64 maxBytes)
{
    if (m_started)
        return true;
    if (maxLinesPerFile <= 0)
        maxLinesPerFile = 10000;

    m_logsDir = logsDir;
    m_maxLinesPerFile = maxLinesPerFile;

    if (!QDir().mkpath(m_logsDir))
        return false;

    // Before this session adds its own file, so the directory settles at
    // keepSessions rather than one more. keepSessions 0 means no count limit.
    pruneSessions(m_logsDir, keepSessions > 0 ? keepSessions - 1 : -1, maxBytes);

    m_sessionStamp = QDateTime::currentDateTime().toString(
        QLatin1String(LogosBasecampPaths::kSessionLogStampFormat));
    m_rotationIndex = 0;
    m_linesInCurrentFile = 0;

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
    const QString name = LogosBasecampPaths::sessionLogFileName(m_sessionStamp, m_rotationIndex);

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
        // Per-chunk flush: QFile buffers otherwise, and a reader tailing the
        // file (Settings → Logs) would see lines seconds late.
        if (m_currentFile)
            m_currentFile->flush();
    }

    if (m_currentFile)
        m_currentFile->flush();
}

#endif // Q_OS_WIN

} // namespace LogosBasecampLog
