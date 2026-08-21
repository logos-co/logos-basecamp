#pragma once

#include "LogLine.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>

class LogFilesModel;
class LogLinesModel;
class LogReaderWorker;
class QTimer;

// LogManager — backend for Settings → Logs.
//
// Owned by MainUIBackend and exposed to QML whole (`backend.logs`), so the
// view talks to one object: a file list, the parsed lines of the open file
// as a model, per-source / per-level tallies for the filter chips, and the
// follow switch. Every read goes through LogReaderWorker on a dedicated
// QThread; this class only owns state and forwards results to the model.
//
// The logs directory is LogosBasecampPaths::logsDirectory(), the same place
// app/utils/LogRedirector writes. The file being written right now is
// published by main.cpp as LOGOS_SESSION_LOG (the plugin library is loaded,
// not linked, so it cannot ask LogRedirector directly).
class LogManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString logsDirectory READ logsDirectory CONSTANT)
    Q_PROPERTY(QString sessionStamp  READ sessionStamp  CONSTANT)
    Q_PROPERTY(QString monospaceFamily READ monospaceFamily CONSTANT)

    // Directory listing, newest session first (see LogFilesModel roles).
    // sessionCount/totalBytes summarise the same listing.
    Q_PROPERTY(LogFilesModel* files READ files CONSTANT)
    Q_PROPERTY(int    sessionCount READ sessionCount NOTIFY filesChanged)
    Q_PROPERTY(double totalBytes   READ totalBytes   NOTIFY filesChanged)

    // The open file.
    Q_PROPERTY(LogLinesModel* lines READ lines CONSTANT)
    Q_PROPERTY(QString currentFile     READ currentFile     NOTIFY currentFileChanged)
    Q_PROPERTY(QString currentFileName READ currentFileName NOTIFY currentFileChanged)
    Q_PROPERTY(bool    currentIsLive   READ currentIsLive   NOTIFY currentFileChanged)
    Q_PROPERTY(bool    loading   READ loading   NOTIFY loadingChanged)
    Q_PROPERTY(bool    truncated READ truncated NOTIFY truncatedChanged)
    Q_PROPERTY(QString error     READ error     NOTIFY errorChanged)
    Q_PROPERTY(QVariantList sourceCounts READ sourceCounts NOTIFY countsChanged)
    Q_PROPERTY(QVariantList levelCounts  READ levelCounts  NOTIFY countsChanged)

    // Follow = poll the live file for appended lines. Only effective while
    // `active` (the view is on screen) and the open file is the live one.
    Q_PROPERTY(bool following READ following WRITE setFollowing NOTIFY followingChanged)
    Q_PROPERTY(bool active    READ active    WRITE setActive    NOTIFY activeChanged)

public:
    explicit LogManager(QObject* parent = nullptr);
    ~LogManager() override;

    QString logsDirectory() const { return m_logsDirectory; }
    QString sessionStamp() const  { return m_sessionStamp; }
    QString monospaceFamily() const { return m_monospaceFamily; }

    LogFilesModel* files() const { return m_files; }
    int    sessionCount() const { return m_sessionCount; }
    double totalBytes() const   { return m_totalBytes; }

    LogLinesModel* lines() const { return m_lines; }
    QString currentFile() const { return m_currentFile; }
    QString currentFileName() const;
    bool    currentIsLive() const;
    bool    loading() const   { return m_loading; }
    bool    truncated() const { return m_truncated; }
    QString error() const     { return m_error; }
    QVariantList sourceCounts() const;
    QVariantList levelCounts() const;

    bool following() const { return m_following; }
    bool active() const    { return m_active; }

public slots:
    void setFollowing(bool following);
    void setActive(bool active);

    // Re-list the directory. Also called on a timer while following so a
    // rotation shows up without a manual reload.
    void refreshFiles();
    // Open `path`; an empty path clears the view.
    void openFile(const QString& path);
    // Open the live file if there is one, else the newest file on disk.
    void openLatest();
    // Reload the open file from scratch.
    void reload();

    void copyText(const QString& text);
    void openLogsFolder();
    // Delete sessions older than the newest `keepSessions`. The running
    // session is never deleted. Result arrives via sessionsDeleted().
    void deleteOlderSessions(int keepSessions);

signals:
    void filesChanged();
    void currentFileChanged();
    void loadingChanged();
    void truncatedChanged();
    void errorChanged();
    void countsChanged();
    void followingChanged();
    void activeChanged();
    void sessionsDeleted(int deletedFiles, int deletedSessions, const QString& error);

private slots:
    void onFilesListed(const QVariantList& files, double totalBytes, int sessionCount);
    void onFileLoaded(int requestId, const QString& path, const QList<LogLine>& lines,
                      bool truncated, const QString& error);
    void onLinesAppended(int requestId, const QString& path, const QList<LogLine>& lines);
    void onFileRestarted(int requestId, const QString& path);
    void onPollTick();

private:
    void setLoading(bool loading);
    void setError(const QString& error);
    void updatePolling();
    // Recompute currentIsLive from the latest listing; emits
    // currentFileChanged only when it moved.
    void refreshLiveFlag();
    // Newest rotation of the running session according to the last listing.
    QString liveFilePath() const;

    QString m_logsDirectory;
    QString m_sessionFile;   // LOGOS_SESSION_LOG, may be empty
    QString m_sessionStamp;
    QString m_monospaceFamily;

    QThread          m_thread;
    LogReaderWorker* m_worker = nullptr;  // lives on m_thread; deleted with it
    QTimer*          m_pollTimer = nullptr;
    int              m_pollTicks = 0;

    LogLinesModel* m_lines = nullptr;
    LogFilesModel* m_files = nullptr;
    int            m_sessionCount = 0;
    double         m_totalBytes = 0;

    QString m_currentFile;
    bool    m_currentIsLive = false;
    int     m_requestId = 0;
    bool    m_loading = false;
    bool    m_truncated = false;
    QString m_error;
    bool    m_following = true;
    bool    m_active = false;
    bool    m_openedOnce = false;
};
