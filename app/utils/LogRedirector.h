#pragma once

#include <QString>
#include <QFile>
#include <atomic>
#include <memory>
#include <thread>

namespace LogosBasecampLog {

// Captures the process stdout/stderr into rotating log files under a given
// directory. One file per session; when the current file hits
// maxLinesPerFile, a suffixed rotation file is opened. Names come from
// LogosBasecampPaths::sessionLogFileName(), which the readers share.
//
// Before the session's first file is opened, the oldest sessions are deleted
// (every rotation of them) until at most `keepSessions - 1` remain and their
// total size is under `maxBytes`, so the directory settles at keepSessions
// sessions once this one is added. 0 disables either limit.
//
// Implementation: replaces stdout/stderr with the write-end of a pipe via
// dup2(); a background reader thread reads from the pipe, writes to the
// current log file (counting newlines for rotation), flushes after every
// chunk so the file can be tailed while the session runs (Settings → Logs
// does exactly that), and mirrors bytes to the original stdout so a terminal
// attached at launch still sees output.
//
// POSIX-only (macOS/Linux). On other platforms start() is a no-op.
class LogRedirector
{
public:
    static LogRedirector& instance();

    // Start capture. Safe to call once; subsequent calls are no-ops.
    // Returns false if redirection could not be set up.
    bool start(const QString& logsDir, int maxLinesPerFile = 10000, int keepSessions = 30,
               qint64 maxBytes = 0);

    // Flush, restore original stdout/stderr, join reader thread, close files.
    void stop();

    // The session's first file (basecamp_<stamp>.log), empty until start()
    // succeeded. Rotations share the stamp, so a reader can find them all.
    QString filePath() const { return m_filePath; }
    QString sessionStamp() const { return m_sessionStamp; }

    // Delete the oldest sessions under `logsDir` until at most `keep` remain
    // (negative: no count limit; 0 deletes all) and their files total at
    // most `maxBytes` (<= 0: no size limit). Shared by start() and exposed
    // for tests. Returns the number of files removed.
    static int pruneSessions(const QString& logsDir, int keep, qint64 maxBytes = 0);

    LogRedirector(const LogRedirector&) = delete;
    LogRedirector& operator=(const LogRedirector&) = delete;

private:
    LogRedirector() = default;
    ~LogRedirector();

    void readerLoop();
    void openNewFile();

    QString m_logsDir;
    QString m_sessionStamp;
    QString m_filePath;
    int m_maxLinesPerFile = 10000;
    int m_rotationIndex = 0;
    int m_linesInCurrentFile = 0;

    std::unique_ptr<QFile> m_currentFile;

    int m_readFd = -1;
    int m_originalStdout = -1;
    int m_originalStderr = -1;

    std::thread m_readerThread;
    std::atomic<bool> m_running{false};
    bool m_started = false;
};

} // namespace LogosBasecampLog
