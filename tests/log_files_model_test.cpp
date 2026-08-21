// srcdeps: LogFilesModel.cpp
//
// Unit tests for LogFilesModel — the file list behind Settings → Logs. The
// interesting part is replaceRows: a listing arrives every few seconds while
// following, and only real changes may disturb the view.

#include "LogFilesModel.h"

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest/QtTest>

namespace {

QVariantMap file(const QString& stamp, int rotation, double size, bool isLive = false,
                 int fileCount = 1)
{
    QVariantMap m;
    m.insert("name", QStringLiteral("basecamp_%1.%2.log").arg(stamp).arg(rotation));
    m.insert("path", QStringLiteral("/logs/basecamp_%1.%2.log").arg(stamp).arg(rotation));
    m.insert("stamp", stamp);
    m.insert("sessionLabel", stamp);
    m.insert("rotation", rotation);
    m.insert("fileCount", fileCount);
    m.insert("size", size);
    m.insert("isCurrentSession", isLive);
    m.insert("isLive", isLive);
    return m;
}

QVariant field(const LogFilesModel& m, int row, LogFilesModel::Roles role)
{
    return m.data(m.index(row), role);
}

} // namespace

class LogFilesModelTest : public QObject {
    Q_OBJECT

private slots:
    void roleNames_stable()
    {
        LogFilesModel model;
        const auto roles = model.roleNames();
        for (const QByteArray& name : { "name", "path", "stamp", "sessionLabel", "rotation",
                                        "fileCount", "size", "modified", "isCurrentSession",
                                        "isLive", "startsSession" })
            QVERIFY2(roles.values().contains(name), name.constData());
    }

    void startsSessionMarksFirstRowOfEachStamp()
    {
        LogFilesModel model;
        model.replaceRows({ file("B", 2, 10, true, 3), file("B", 1, 10, false, 3),
                            file("B", 0, 10, false, 3), file("A", 0, 5) });
        QCOMPARE(model.count(), 4);
        QCOMPARE(field(model, 0, LogFilesModel::StartsSessionRole).toBool(), true);
        QCOMPARE(field(model, 1, LogFilesModel::StartsSessionRole).toBool(), false);
        QCOMPARE(field(model, 2, LogFilesModel::StartsSessionRole).toBool(), false);
        QCOMPARE(field(model, 3, LogFilesModel::StartsSessionRole).toBool(), true);
        QCOMPARE(model.livePath(), QString("/logs/basecamp_B.2.log"));
        QCOMPARE(model.firstPath(), QString("/logs/basecamp_B.2.log"));
    }

    void samePathsPatchInPlace()
    {
        LogFilesModel model;
        model.replaceRows({ file("B", 0, 10, true), file("A", 0, 5) });
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);

        // Only the live file grew: one dataChanged for one role, no reset.
        model.replaceRows({ file("B", 0, 12, true), file("A", 0, 5) });
        QCOMPARE(resets.count(), 0);
        QCOMPARE(changes.count(), 1);
        QCOMPARE(changes.first().at(0).toModelIndex().row(), 0);
        QCOMPARE(changes.first().at(2).value<QList<int>>(), QList<int>{ LogFilesModel::SizeRole });
        QCOMPARE(field(model, 0, LogFilesModel::SizeRole).toDouble(), 12.0);

        // Identical listing: nothing emitted at all.
        model.replaceRows({ file("B", 0, 12, true), file("A", 0, 5) });
        QCOMPARE(resets.count(), 0);
        QCOMPARE(changes.count(), 1);
    }

    void differentPathsReset()
    {
        LogFilesModel model;
        model.replaceRows({ file("B", 0, 10, true), file("A", 0, 5) });
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy counts(&model, &LogFilesModel::countChanged);

        // A rotation appeared at the top: the set of paths changed.
        model.replaceRows({ file("B", 1, 1, true, 2), file("B", 0, 10, false, 2), file("A", 0, 5) });
        QCOMPARE(resets.count(), 1);
        QCOMPARE(counts.count(), 1);
        QCOMPARE(model.count(), 3);
        QCOMPARE(model.livePath(), QString("/logs/basecamp_B.1.log"));
        QCOMPARE(field(model, 1, LogFilesModel::IsLiveRole).toBool(), false);
    }

    void getReturnsRoleMap()
    {
        LogFilesModel model;
        model.replaceRows({ file("A", 0, 5) });
        const QVariantMap m = model.get(0);
        QCOMPARE(m.value("path").toString(), QString("/logs/basecamp_A.0.log"));
        QCOMPARE(m.value("startsSession").toBool(), true);
        QVERIFY(model.get(1).isEmpty());
        QVERIFY(model.get(-1).isEmpty());
    }
};

QTEST_GUILESS_MAIN(LogFilesModelTest)
#include "log_files_model_test.moc"
