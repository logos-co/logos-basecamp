// srcdeps: ../app/utils/LogRedirector.cpp
//
// Unit tests for LogRedirector::pruneSessions — the launch-time retention
// behind Settings → Logs. It deletes user files, so the rules get pinned
// down against a temp directory: oldest sessions first, whole sessions
// only, count and size limits independently and together, unrelated files
// untouched.

#include "LogRedirector.h"
#include "LogosBasecampPaths.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using LogosBasecampLog::LogRedirector;

namespace {

// Create a session with `rotations + 1` files of `bytesPerFile` each.
void makeSession(const QDir& dir, const QString& stamp, int rotations, int bytesPerFile)
{
    for (int r = 0; r <= rotations; ++r) {
        QFile f(dir.filePath(LogosBasecampPaths::sessionLogFileName(stamp, r)));
        QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(f.fileName()));
        f.write(QByteArray(bytesPerFile, 'x'));
    }
}

QStringList sessionsIn(const QDir& dir)
{
    QSet<QString> stamps;
    const QStringList names = dir.entryList({ QLatin1String(LogosBasecampPaths::kSessionLogGlob) },
                                            QDir::Files, QDir::Name);
    for (const QString& n : names) {
        QString stamp;
        if (LogosBasecampPaths::parseSessionLogFileName(n, &stamp, nullptr)) stamps.insert(stamp);
    }
    QStringList out(stamps.cbegin(), stamps.cend());
    out.sort();
    return out;
}

} // namespace

class LogRetentionTest : public QObject {
    Q_OBJECT

private slots:
    void countLimitDeletesOldestWholeSessions()
    {
        QTemporaryDir tmp;
        const QDir dir(tmp.path());
        makeSession(dir, "20260801_000000", 2, 10);   // 3 files
        makeSession(dir, "20260802_000000", 0, 10);
        makeSession(dir, "20260803_000000", 1, 10);   // 2 files
        makeSession(dir, "20260804_000000", 0, 10);
        QFile other(dir.filePath("notes.txt"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.close();

        QCOMPARE(LogRedirector::pruneSessions(dir.path(), 2), 4);   // 3 + 1 files
        QCOMPARE(sessionsIn(dir), QStringList({ "20260803_000000", "20260804_000000" }));
        QVERIFY(QFile::exists(dir.filePath("notes.txt")));

        // Already within the limit: nothing happens.
        QCOMPARE(LogRedirector::pruneSessions(dir.path(), 2), 0);
        // keep 0 empties the directory of sessions.
        QCOMPARE(LogRedirector::pruneSessions(dir.path(), 0), 3);
        QVERIFY(sessionsIn(dir).isEmpty());
    }

    void noLimitsDeletesNothing()
    {
        QTemporaryDir tmp;
        const QDir dir(tmp.path());
        makeSession(dir, "20260801_000000", 0, 10);
        QCOMPARE(LogRedirector::pruneSessions(dir.path(), -1, 0), 0);
        QCOMPARE(sessionsIn(dir).size(), 1);
    }

    void sizeLimitDeletesOldestUntilUnderCap()
    {
        QTemporaryDir tmp;
        const QDir dir(tmp.path());
        makeSession(dir, "20260801_000000", 0, 100);
        makeSession(dir, "20260802_000000", 3, 100);   // 400 bytes
        makeSession(dir, "20260803_000000", 0, 100);
        // 600 bytes total, cap 250: drop 0801 (500 left), then 0802 (100 left).
        QCOMPARE(LogRedirector::pruneSessions(dir.path(), -1, 250), 5);
        QCOMPARE(sessionsIn(dir), QStringList({ "20260803_000000" }));
    }

    void sizeLimitCanTakeTheNewestSessionToo()
    {
        // The cap is the cap: at launch the newest existing session is the
        // previous one, and if it alone exceeds the cap it goes as well.
        QTemporaryDir tmp;
        const QDir dir(tmp.path());
        makeSession(dir, "20260801_000000", 9, 100);   // 1000 bytes
        QCOMPARE(LogRedirector::pruneSessions(dir.path(), -1, 500), 10);
        QVERIFY(sessionsIn(dir).isEmpty());
    }

    void bothLimitsWhicheverBitesFirst()
    {
        QTemporaryDir tmp;
        const QDir dir(tmp.path());
        makeSession(dir, "20260801_000000", 0, 10);
        makeSession(dir, "20260802_000000", 0, 10);
        makeSession(dir, "20260803_000000", 0, 10);
        makeSession(dir, "20260804_000000", 0, 10);
        // Count would keep 3; size (25 bytes) only allows 2.
        QCOMPARE(LogRedirector::pruneSessions(dir.path(), 3, 25), 2);
        QCOMPARE(sessionsIn(dir), QStringList({ "20260803_000000", "20260804_000000" }));
    }

    void missingDirectoryIsHarmless()
    {
        QCOMPARE(LogRedirector::pruneSessions("/nonexistent/basecamp-logs-test", 1, 1), 0);
    }
};

QTEST_GUILESS_MAIN(LogRetentionTest)
#include "log_retention_test.moc"
