// srcdeps: utils/InstalledIntegrity.cpp utils/InstalledPlacement.cpp
// libdeps: lgx package_manager_lib
//
// The launch-time integrity check examines only the copy of a core module that
// will run, as copyThatRuns() predicts it. This holds that prediction to the
// package manager library liblogos links, scanned the way liblogos scans: every
// modules dir, in search order, each added as an embedded one.

#include "../app/utils/InstalledIntegrity.h"

#include <package_manager_lib.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using logos::integrity::copyThatRuns;

class CopyThatRunsTest : public QObject
{
    Q_OBJECT

    QTemporaryDir tmp;
    int serial = 0;

    static bool put(const QString& modulesDir, const QByteArray& version)
    {
        if (version.isEmpty())
            return true;
        if (!QDir().mkpath(modulesDir + QStringLiteral("/mod")))
            return false;
        QFile file(modulesDir + QStringLiteral("/mod/manifest.json"));
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && file.write("{\"name\":\"mod\",\"type\":\"core\",\"version\":\"" + version + "\"}") > 0;
    }

    // liblogos's ModuleRegistry: the first dir set, the others added after it.
    static QString coreRuns(const QStringList& modulesDirs)
    {
        PackageManagerLib pm;
        pm.setEmbeddedModulesDirectory(modulesDirs.first().toStdString());
        for (int i = 1; i < modulesDirs.size(); ++i)
            pm.addEmbeddedModulesDirectory(modulesDirs.at(i).toStdString());
        for (const InstalledPackage& package : pm.getInstalledModules())
            if (package.name == "mod")
                return QDir::cleanPath(QString::fromStdString(package.installDir));
        return {};
    }

private slots:
    void predictsTheCopyTheCoreRuns_data()
    {
        QTest::addColumn<QByteArray>("bundled");
        QTest::addColumn<QByteArray>("user");
        QTest::addColumn<QByteArray>("profile");
        QTest::addColumn<QString>("runs");

        QTest::newRow("bundled only") << QByteArray("1.0.0") << QByteArray() << QByteArray() << "bundled";
        QTest::newRow("user only") << QByteArray() << QByteArray("1.0.0") << QByteArray() << "user";
        QTest::newRow("a tie goes to the later") << QByteArray("1.0.0") << QByteArray("1.0.0") << QByteArray() << "user";
        QTest::newRow("newer user copy") << QByteArray("1.0.0") << QByteArray("1.1.0") << QByteArray() << "user";
        QTest::newRow("older user copy") << QByteArray("1.2.0") << QByteArray("1.1.0") << QByteArray() << "bundled";
        QTest::newRow("junk sorts lowest") << QByteArray("1.0.0") << QByteArray("not-a-version") << QByteArray() << "bundled";
        QTest::newRow("older profile copy") << QByteArray("1.0.0") << QByteArray("2.0.0") << QByteArray("1.5.0") << "user";
        QTest::newRow("profile tie goes to the profile") << QByteArray("1.0.0") << QByteArray("2.0.0") << QByteArray("2.0.0") << "profile";
    }

    void predictsTheCopyTheCoreRuns()
    {
        QFETCH(QByteArray, bundled);
        QFETCH(QByteArray, user);
        QFETCH(QByteArray, profile);
        QFETCH(QString, runs);

        const QString base = tmp.filePath(QStringLiteral("copies%1").arg(++serial));
        QStringList dirs;
        for (const QString& dir : {QStringLiteral("bundled"), QStringLiteral("user"), QStringLiteral("profile")}) {
            dirs << base + QLatin1Char('/') + dir;
            QVERIFY(QDir().mkpath(dirs.last()));
        }
        QVERIFY(put(dirs.at(0), bundled));
        QVERIFY(put(dirs.at(1), user));
        QVERIFY(put(dirs.at(2), profile));

        const QString expected = QDir::cleanPath(base + QLatin1Char('/') + runs + QStringLiteral("/mod"));
        QCOMPARE(coreRuns(dirs), expected);
        QCOMPARE(QDir::cleanPath(copyThatRuns(QStringLiteral("mod"), dirs)), expected);
    }
};

QTEST_GUILESS_MAIN(CopyThatRunsTest)
#include "copy_that_runs_test.moc"
