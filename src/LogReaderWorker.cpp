#include "LogReaderWorker.h"

#include "LogosBasecampPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QVariantMap>

#include <algorithm>

LogReaderWorker::LogReaderWorker(QObject* parent)
    : QObject(parent)
{
}

namespace {

struct FileEntry {
    QString name;
    QString path;
    QString stamp;
    int     rotation = 0;
    qint64  size = 0;
    QDateTime modified;
};

QString sessionLabel(const QString& stamp)
{
    const QDateTime dt = QDateTime::fromString(
        stamp, QLatin1String(LogosBasecampPaths::kSessionLogStampFormat));
    return dt.isValid() ? dt.toString(QStringLiteral("d MMM yyyy HH:mm:ss")) : stamp;
}

QFileInfoList sessionLogFiles(const QString& dir)
{
    return QDir(dir).entryInfoList(
        { QLatin1String(LogosBasecampPaths::kSessionLogGlob) },
        QDir::Files | QDir::NoSymLinks, QDir::Name);
}

} // namespace

void LogReaderWorker::listFiles(const QString& dir, const QString& liveStamp)
{
    QList<FileEntry> entries;
    QMap<QString, int> maxRotation;

    const QFileInfoList infos = sessionLogFiles(dir);
    double totalBytes = 0;
    for (const QFileInfo& fi : infos) {
        FileEntry e;
        if (!LogosBasecampPaths::parseSessionLogFileName(fi.fileName(), &e.stamp, &e.rotation)) continue;
        e.name = fi.fileName();
        e.path = fi.absoluteFilePath();
        e.size = fi.size();
        e.modified = fi.lastModified();
        totalBytes += e.size;
        maxRotation[e.stamp] = std::max(maxRotation.value(e.stamp, 0), e.rotation);
        entries.append(e);
    }

    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.stamp != b.stamp) return a.stamp > b.stamp;
        return a.rotation > b.rotation;
    });

    QVariantList out;
    out.reserve(entries.size());
    for (const FileEntry& e : entries) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), e.name);
        m.insert(QStringLiteral("path"), e.path);
        m.insert(QStringLiteral("stamp"), e.stamp);
        m.insert(QStringLiteral("sessionLabel"), sessionLabel(e.stamp));
        m.insert(QStringLiteral("rotation"), e.rotation);
        // Files in this session, from its highest rotation index — so a
        // directory with gaps still reads "file 522 of 523".
        m.insert(QStringLiteral("fileCount"), maxRotation.value(e.stamp, 0) + 1);
        m.insert(QStringLiteral("size"), static_cast<double>(e.size));
        m.insert(QStringLiteral("modified"), e.modified);
        m.insert(QStringLiteral("isCurrentSession"), !liveStamp.isEmpty() && e.stamp == liveStamp);
        // The file the running session is writing right now: newest rotation
        // of the live stamp. Only this one is worth following.
        m.insert(QStringLiteral("isLive"),
                 !liveStamp.isEmpty() && e.stamp == liveStamp
                     && e.rotation == maxRotation.value(e.stamp, 0));
        out.append(m);
    }

    emit filesListed(out, totalBytes, maxRotation.size());
}

QList<LogLine> LogReaderWorker::consume(const QByteArray& data)
{
    m_carry.append(data);
    const int lastNl = m_carry.lastIndexOf('\n');
    if (lastNl < 0) return {};

    const QString complete = QString::fromUtf8(m_carry.constData(), lastNl);
    m_carry.remove(0, lastNl + 1);
    return LogLineParser::parseText(complete);
}

void LogReaderWorker::loadFile(const QString& path, int requestId, bool expectGrowth)
{
    m_tailPath.clear();
    m_tailOffset = 0;
    m_carry.clear();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        emit fileLoaded(requestId, path, {}, false,
                        tr("Could not open %1: %2").arg(path, f.errorString()));
        return;
    }

    bool truncated = false;
    const qint64 size = f.size();
    if (size > kMaxReadBytes) {
        truncated = true;
        f.seek(size - kMaxReadBytes);
        // Drop the partial line we landed in.
        f.readLine();
    }

    const QByteArray data = f.readAll();
    m_tailPath = path;
    m_tailOffset = f.pos();
    QList<LogLine> lines = consume(data);
    if (!m_carry.isEmpty() && !expectGrowth) {
        // Nothing will finish this line (crash, kill -9, ...): show what's there.
        lines.append(LogLineParser::parse(QString::fromUtf8(m_carry)));
    } else {
        m_tailOffset -= m_carry.size();
    }
    m_carry.clear();

    emit fileLoaded(requestId, path, lines, truncated, QString());
}

void LogReaderWorker::pollFile(const QString& path, int requestId)
{
    if (path != m_tailPath) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;

    const qint64 size = f.size();
    if (size < m_tailOffset) {
        // Shrunk or replaced: the owner reloads from scratch (loadFile resets
        // the tail state); appending from 0 would duplicate what is shown.
        m_tailPath.clear();
        m_tailOffset = 0;
        m_carry.clear();
        emit fileRestarted(requestId, path);
        return;
    }
    if (size == m_tailOffset) return;

    f.seek(m_tailOffset);
    const QByteArray data = f.readAll();
    const QList<LogLine> lines = consume(data);
    // An unterminated tail is re-read on the next poll together with its line
    // ending, so rewind the offset past it rather than carrying bytes across.
    m_tailOffset = f.pos() - m_carry.size();
    m_carry.clear();

    if (!lines.isEmpty())
        emit linesAppended(requestId, path, lines);
}

void LogReaderWorker::deleteSessions(const QString& dir, int keepSessions, const QString& protectStamp)
{
    const QFileInfoList infos = sessionLogFiles(dir);

    QSet<QString> stamps;
    for (const QFileInfo& fi : infos) {
        QString stamp;
        if (LogosBasecampPaths::parseSessionLogFileName(fi.fileName(), &stamp, nullptr))
            stamps.insert(stamp);
    }

    QStringList ordered(stamps.cbegin(), stamps.cend());
    std::sort(ordered.begin(), ordered.end(), std::greater<QString>());

    QSet<QString> doomed;
    int kept = 0;
    for (const QString& s : ordered) {
        if (s == protectStamp || kept < keepSessions) { ++kept; continue; }
        doomed.insert(s);
    }

    int deletedFiles = 0;
    QString error;
    for (const QFileInfo& fi : infos) {
        QString stamp;
        if (!LogosBasecampPaths::parseSessionLogFileName(fi.fileName(), &stamp, nullptr)) continue;
        if (!doomed.contains(stamp)) continue;
        if (QFile::remove(fi.absoluteFilePath())) ++deletedFiles;
        else if (error.isEmpty()) error = tr("Could not delete %1").arg(fi.fileName());
    }

    emit sessionsDeleted(deletedFiles, doomed.size(), error);
}
