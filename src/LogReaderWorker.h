#pragma once

#include "LogLine.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

// All file I/O for Settings → Logs, run on LogManager's worker thread so a
// 4 MB rotation file or a 2,000-entry directory listing never stalls the UI
// thread. Slots are invoked through the queued meta-call machinery; results
// come back as signals carrying plain values.
//
// `requestId` is echoed back so LogManager can drop results for a file the
// user has already navigated away from.
class LogReaderWorker : public QObject {
    Q_OBJECT
public:
    explicit LogReaderWorker(QObject* parent = nullptr);

    // Files larger than this are read from the tail only; the first partial
    // line is dropped. Rotation keeps real session files far below it, so
    // this only guards against a runaway single file.
    static constexpr qint64 kMaxReadBytes = 32 * 1024 * 1024;

public slots:
    // Enumerate basecamp_*.log files under `dir`, newest session first,
    // highest rotation first within a session.
    void listFiles(const QString& dir, const QString& liveStamp);

    // Read and parse the whole file. Remembers the end offset so pollFile can
    // continue from there. With `expectGrowth` an unterminated last line is
    // held back until its newline arrives; without it (a finished file) it is
    // emitted as the final line.
    void loadFile(const QString& path, int requestId, bool expectGrowth);

    // Read whatever was appended since loadFile/pollFile last looked. No-op
    // when `path` is not the file loadFile last loaded.
    void pollFile(const QString& path, int requestId);

    // Delete every session older than the newest `keepSessions`, never
    // touching `protectStamp` (the running session).
    void deleteSessions(const QString& dir, int keepSessions, const QString& protectStamp);

signals:
    void filesListed(const QVariantList& files, double totalBytes, int sessionCount);
    void fileLoaded(int requestId, const QString& path, const QList<LogLine>& lines,
                    bool truncated, const QString& error);
    void linesAppended(int requestId, const QString& path, const QList<LogLine>& lines);
    // The tailed file is smaller than where we left off (replaced or
    // truncated); the owner should reload it instead of appending.
    void fileRestarted(int requestId, const QString& path);
    void sessionsDeleted(int deletedFiles, int deletedSessions, const QString& error);

private:
    // Consume complete lines from m_carry + data; keep the unterminated tail.
    QList<LogLine> consume(const QByteArray& data);

    QString    m_tailPath;
    qint64     m_tailOffset = 0;
    QByteArray m_carry;
};
