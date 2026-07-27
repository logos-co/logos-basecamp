#pragma once

#include <QString>
#include <QFile>
#include <atomic>
#include <memory>
#include <thread>

namespace LogosBasecampLog {

// Captures the process stdout/stderr into rotating log files under a given
// directory. One file per session (named basecamp_YYYYMMDD_HHMMSS.log); when
// the current file hits maxLinesPerFile, a suffixed rotation file is opened
// (basecamp_YYYYMMDD_HHMMSS.001.log, .002.log, ...). Only the most recent
// keepSessions sessions are kept; older ones are deleted when a session starts.
//
// Implementation: replaces stdout/stderr with the write-end of a pipe via
// dup2(); a background reader thread reads from the pipe, writes to the
// current log file (counting newlines for rotation) and mirrors bytes to the
// original stdout so a terminal attached at launch still sees output.
//
// POSIX-only (macOS/Linux). On other platforms start() is a no-op.
class LogRedirector
{
public:
    static LogRedirector& instance();

    // Start capture. Safe to call once; subsequent calls are no-ops.
    // Returns false if redirection could not be set up.
    bool start(const QString& logsDir, int maxLinesPerFile = 10000, int keepSessions = 10);

    // Flush, restore original stdout/stderr, join reader thread, close files.
    void stop();

    // The session's first file, empty until start() has succeeded. It stays
    // addressable across rotations, which the current file does not.
    QString filePath() const;

    LogRedirector(const LogRedirector&) = delete;
    LogRedirector& operator=(const LogRedirector&) = delete;

private:
    LogRedirector() = default;
    ~LogRedirector();

    void readerLoop();
    void openNewFile();
    void pruneOlderSessions();

    QString m_logsDir;
    QString m_sessionStamp;
    QString m_filePath;
    int m_maxLinesPerFile = 10000;
    int m_keepSessions = 10;
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
