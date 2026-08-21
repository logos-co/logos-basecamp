#include "LogManager.h"

#include "LogFilesModel.h"
#include "LogLinesModel.h"
#include "LogReaderWorker.h"
#include "LogosBasecampPaths.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

namespace {
constexpr int kPollIntervalMs = 1000;
// Re-list the directory every Nth poll so a rotation of the live file is
// picked up without the user pressing Reload.
constexpr int kListEveryTicks = 5;
}

LogManager::LogManager(QObject* parent)
    : QObject(parent)
    , m_logsDirectory(LogosBasecampPaths::logsDirectory())
    , m_sessionFile(QString::fromUtf8(qgetenv(LogosBasecampPaths::kSessionLogEnvVar)))
    , m_monospaceFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family())
    , m_lines(new LogLinesModel(this))
    , m_files(new LogFilesModel(this))
{
    qRegisterMetaType<LogLine>("LogLine");
    qRegisterMetaType<QList<LogLine>>("QList<LogLine>");

    if (!m_sessionFile.isEmpty())
        LogosBasecampPaths::parseSessionLogFileName(QFileInfo(m_sessionFile).fileName(),
                                                    &m_sessionStamp, nullptr);

    m_worker = new LogReaderWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &LogReaderWorker::filesListed,     this, &LogManager::onFilesListed);
    connect(m_worker, &LogReaderWorker::fileLoaded,      this, &LogManager::onFileLoaded);
    connect(m_worker, &LogReaderWorker::linesAppended,   this, &LogManager::onLinesAppended);
    connect(m_worker, &LogReaderWorker::fileRestarted,   this, &LogManager::onFileRestarted);
    connect(m_worker, &LogReaderWorker::sessionsDeleted, this, &LogManager::sessionsDeleted);
    connect(m_worker, &LogReaderWorker::sessionsDeleted, this,
            [this](int, int, const QString&) { refreshFiles(); });
    m_thread.setObjectName(QStringLiteral("basecamp-log-reader"));
    m_thread.start();

    connect(m_lines, &LogLinesModel::countsChanged, this, &LogManager::countsChanged);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &LogManager::onPollTick);
}

LogManager::~LogManager()
{
    m_thread.quit();
    m_thread.wait();
}

int LogManager::defaultKeepSessions() const
{
    return LogosBasecampPaths::kDefaultKeepSessions;
}

QString LogManager::currentFileName() const
{
    return m_currentFile.isEmpty() ? QString() : QFileInfo(m_currentFile).fileName();
}

bool LogManager::currentIsLive() const
{
    return m_currentIsLive;
}

void LogManager::refreshLiveFlag()
{
    const bool live = !m_currentFile.isEmpty() && m_currentFile == liveFilePath();
    if (live == m_currentIsLive) return;
    m_currentIsLive = live;
    emit currentFileChanged();
}

QVariantList LogManager::sourceCounts() const { return m_lines->sourceCounts(); }
QVariantList LogManager::levelCounts() const  { return m_lines->levelCounts(); }

QString LogManager::liveFilePath() const
{
    return m_files->livePath();
}

void LogManager::setFollowing(bool following)
{
    if (m_following == following) return;
    m_following = following;
    emit followingChanged();
    updatePolling();
}

void LogManager::setActive(bool active)
{
    if (m_active == active) return;
    m_active = active;
    emit activeChanged();
    if (m_active) {
        refreshFiles();
        if (!m_openedOnce) {
            // First time on screen: the live file, once the listing says
            // which one that is (see onFilesListed).
            m_openedOnce = true;
        } else if (!m_currentFile.isEmpty()) {
            // Catch up on what was appended while the page was hidden.
            onPollTick();
        }
    }
    updatePolling();
}

void LogManager::updatePolling()
{
    const bool shouldPoll = m_active && m_following && currentIsLive();
    if (shouldPoll && !m_pollTimer->isActive()) {
        m_pollTicks = 0;
        m_pollTimer->start();
    } else if (!shouldPoll && m_pollTimer->isActive()) {
        m_pollTimer->stop();
    }
}

void LogManager::refreshFiles()
{
    const QString dir = m_logsDirectory;
    const QString stamp = m_sessionStamp;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, dir, stamp]() {
        w->listFiles(dir, stamp);
    }, Qt::QueuedConnection);
}

void LogManager::openFile(const QString& path)
{
    ++m_requestId;
    setError(QString());
    if (m_truncated) { m_truncated = false; emit truncatedChanged(); }

    const bool live = !path.isEmpty() && path == liveFilePath();
    if (m_currentFile != path || m_currentIsLive != live) {
        m_currentFile = path;
        m_currentIsLive = live;
        emit currentFileChanged();
    }
    m_lines->clear();

    if (path.isEmpty()) {
        setLoading(false);
        updatePolling();
        return;
    }

    setLoading(true);
    const int id = m_requestId;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, path, id, live]() {
        w->loadFile(path, id, live);
    }, Qt::QueuedConnection);
    updatePolling();
}

void LogManager::openLatest()
{
    QString path = liveFilePath();
    if (path.isEmpty()) path = m_files->firstPath();
    if (!path.isEmpty()) openFile(path);
}

void LogManager::reload()
{
    refreshFiles();
    if (!m_currentFile.isEmpty()) openFile(m_currentFile);
}

void LogManager::copyText(const QString& text)
{
    if (QClipboard* cb = QGuiApplication::clipboard()) cb->setText(text);
}

void LogManager::openLogsFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_logsDirectory));
}

void LogManager::deleteOlderSessions(int keepSessions)
{
    const QString dir = m_logsDirectory;
    const QString stamp = m_sessionStamp;
    const int keep = keepSessions < 1 ? 1 : keepSessions;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, dir, keep, stamp]() {
        w->deleteSessions(dir, keep, stamp);
    }, Qt::QueuedConnection);
}

void LogManager::onFilesListed(const QVariantList& files, double totalBytes, int sessionCount)
{
    const QString previousLive = liveFilePath();
    m_files->replaceRows(files);
    if (m_totalBytes != totalBytes || m_sessionCount != sessionCount) {
        m_totalBytes = totalBytes;
        m_sessionCount = sessionCount;
        emit filesChanged();
    }

    const QString live = liveFilePath();

    if (m_currentFile.isEmpty() && m_openedOnce && m_active) {
        openLatest();
        return;
    }

    // The live file rotated underneath us: hop to the new rotation so
    // "follow" keeps meaning "what the app is writing now".
    if (m_following && m_active && !previousLive.isEmpty()
        && m_currentFile == previousLive && live != previousLive && !live.isEmpty()) {
        openFile(live);
        return;
    }

    // The open file may have stopped (or started) being the live one.
    refreshLiveFlag();
    updatePolling();
}

void LogManager::onFileLoaded(int requestId, const QString& path, const QList<LogLine>& lines,
                              bool truncated, const QString& error)
{
    if (requestId != m_requestId || path != m_currentFile) return;

    m_lines->reset(lines);
    if (m_truncated != truncated) { m_truncated = truncated; emit truncatedChanged(); }
    setError(error);
    setLoading(false);
    updatePolling();
}

void LogManager::onLinesAppended(int requestId, const QString& path, const QList<LogLine>& lines)
{
    if (requestId != m_requestId || path != m_currentFile) return;
    m_lines->append(lines);
}

void LogManager::onFileRestarted(int requestId, const QString& path)
{
    // The file shrank under us (replaced or truncated): reload rather than
    // append a second copy of lines already shown.
    if (requestId != m_requestId || path != m_currentFile) return;
    openFile(path);
}

void LogManager::onPollTick()
{
    if (m_currentFile.isEmpty()) return;
    const QString path = m_currentFile;
    const int id = m_requestId;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, path, id]() {
        w->pollFile(path, id);
    }, Qt::QueuedConnection);

    if (++m_pollTicks % kListEveryTicks == 0) refreshFiles();
}

void LogManager::setLoading(bool loading)
{
    if (m_loading == loading) return;
    m_loading = loading;
    emit loadingChanged();
}

void LogManager::setError(const QString& error)
{
    if (m_error == error) return;
    m_error = error;
    emit errorChanged();
}
