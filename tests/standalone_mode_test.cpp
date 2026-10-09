// srcdeps: utils/StandaloneMode.cpp utils/HostProfile.cpp
//
// Unit tests for building the dev-host HostProfile from `--module` /
// `--modules-dir` (app/utils/StandaloneMode.{h,cpp}, app/utils/HostProfile.h):
// which roots the scanners get, which apps open, and which capabilities the
// profile turns off. Run: nix build .#unit-tests -L

#include "../app/utils/StandaloneMode.h"
#include "ShellSections.h"

#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using LogosBasecamp::HostProfile;

namespace {

// Lays out <root>/<sub>/<name>/manifest.json the way lgpm installs a module.
QString installModule(const QString& root, const QString& sub,
                      const QString& name, const QString& type)
{
    const QString dir = QDir(root).filePath(sub.isEmpty() ? name : sub + "/" + name);
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(QStringLiteral("manifest.json")));
    f.open(QIODevice::WriteOnly);
    f.write(QStringLiteral(R"({"name":"%1","type":"%2"})").arg(name, type).toUtf8());
    return dir;
}

QString canon(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

class StandaloneModeTest : public QObject
{
    Q_OBJECT

private slots:
    void installRootOpensItsUiAppsAndScansBothDirs()
    {
        // What `nix build .#install` produces.
        QTemporaryDir tmp;
        installModule(tmp.path(), "plugins", "my_ui", "ui_qml");
        installModule(tmp.path(), "modules", "my_core", "core");

        HostProfile p;
        const auto r = LogosBasecamp::addStandaloneModule(p, tmp.path());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(p.appsToOpen, QStringList{"my_ui"});
        QCOMPARE(p.uiPluginRoots, QStringList{canon(tmp.path() + "/plugins")});
        QCOMPARE(p.coreModuleRoots, QStringList{canon(tmp.path() + "/modules")});
    }

    void singleModuleDirIsRefused()
    {
        // Scanning a module dir means scanning its parent, which would expose
        // the sibling — and let it satisfy a dependency the app never shipped.
        QTemporaryDir tmp;
        const QString ui = installModule(tmp.path(), "plugins", "my_ui", "ui_qml");
        installModule(tmp.path(), "plugins", "sibling_ui", "ui_qml");
        const QString core = installModule(tmp.path(), "modules", "my_core", "core");
        installModule(tmp.path(), "modules", "sibling_core", "core");

        HostProfile p;
        const auto r = LogosBasecamp::addStandaloneModule(p, ui);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("install root")));
        QVERIFY(!LogosBasecamp::addStandaloneModule(p, core).ok);
        QVERIFY(p.appsToOpen.isEmpty());
        QVERIFY(p.uiPluginRoots.isEmpty());
        QVERIFY(p.coreModuleRoots.isEmpty());
    }

    void appsOpenInCliOrderWithoutDuplicates()
    {
        QTemporaryDir b, a;
        installModule(b.path(), "plugins", "b_ui", "ui_qml");
        installModule(a.path(), "plugins", "a_ui", "ui_qml");

        HostProfile p;
        QVERIFY(LogosBasecamp::addStandaloneModule(p, b.path()).ok);
        QVERIFY(LogosBasecamp::addStandaloneModule(p, a.path()).ok);
        QVERIFY(LogosBasecamp::addStandaloneModule(p, b.path()).ok);
        QCOMPARE(p.appsToOpen, (QStringList{"b_ui", "a_ui"}));
        // Re-adding a root moves it last: a later root wins a name clash.
        QCOMPARE(p.uiPluginRoots, (QStringList{canon(a.path() + "/plugins"),
                                               canon(b.path() + "/plugins")}));
    }

    void moduleRootsWinOverDependencyRoots()
    {
        // main() adds --modules-dir first, then --module.
        QTemporaryDir deps, app;
        installModule(deps.path(), "plugins", "my_ui", "ui_qml");
        installModule(app.path(), "plugins", "my_ui", "ui_qml");

        HostProfile p;
        QVERIFY(LogosBasecamp::addStandaloneModulesDir(p, deps.path()).ok);
        QVERIFY(LogosBasecamp::addStandaloneModule(p, app.path()).ok);
        QCOMPARE(p.uiPluginRoots.last(), canon(app.path() + "/plugins"));
    }

    void modulesDirMakesAppsAvailableButOpensNone()
    {
        QTemporaryDir tmp;
        installModule(tmp.path(), "plugins", "provider_ui", "ui_qml");

        HostProfile p;
        QVERIFY(LogosBasecamp::addStandaloneModulesDir(p, tmp.path()).ok);
        QVERIFY(p.appsToOpen.isEmpty());
        QCOMPARE(p.uiPluginRoots, QStringList{canon(tmp.path() + "/plugins")});
    }

    void flatModulesDirGoesToBothScans()
    {
        QTemporaryDir tmp;
        installModule(tmp.path(), QString(), "dep_module", "core");

        HostProfile p;
        QVERIFY(LogosBasecamp::addStandaloneModulesDir(p, tmp.path()).ok);
        QCOMPARE(p.coreModuleRoots, QStringList{canon(tmp.path())});
        QCOMPARE(p.uiPluginRoots, QStringList{canon(tmp.path())});
    }

    void rejectsWhatIsNotAnInstallRoot()
    {
        QTemporaryDir tmp;
        HostProfile p;
        QVERIFY(!LogosBasecamp::addStandaloneModule(p, tmp.path() + "/missing").ok);
        const auto r = LogosBasecamp::addStandaloneModule(p, tmp.path());
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral(".#install")));
        QVERIFY(!LogosBasecamp::addStandaloneModulesDir(p, tmp.path() + "/missing").ok);
    }

    void desktopProfileOffersEverySection()
    {
        const HostProfile p;
        QCOMPARE(p.availableSections(),
                 (QList<int>{ShellSection::Workspace, ShellSection::AppManager,
                             ShellSection::PackageManager, ShellSection::Settings}));
        QVERIFY(p.useUserModules && p.packageCatalog && p.registerUrlScheme && p.singleInstance);
        QVERIFY(!p.devHost);
    }

    void standaloneProfileDropsCatalogueAndUserState()
    {
        HostProfile p;
        LogosBasecamp::applyStandaloneCapabilities(p);
        QCOMPARE(p.availableSections(),
                 (QList<int>{ShellSection::Workspace, ShellSection::Settings}));
        QVERIFY(p.launcherLimitedToOpenedApps);
        QVERIFY(!p.useUserModules);
        QVERIFY(!p.packageCatalog);
        QVERIFY(!p.repositoryManagement);
        QVERIFY(!p.registerUrlScheme);
        QVERIFY(!p.singleInstance);
        QVERIFY(p.defaultUserDir.endsWith(QStringLiteral(".logos-basecamp-dev")));
        QCOMPARE(p.applicationName, QStringLiteral("LogosBasecampStandalone"));
        QVERIFY(p.devHost);
    }

    void qmlSourceNeedsNameAndExistingDir()
    {
        QTemporaryDir tmp;
        HostProfile c;
        QVERIFY(LogosBasecamp::addStandaloneQmlSource(c, "my_ui=" + tmp.path()).ok);
        QCOMPARE(c.qmlSources.value("my_ui"), canon(tmp.path()));

        QVERIFY(!LogosBasecamp::addStandaloneQmlSource(c, tmp.path()).ok);
        QVERIFY(!LogosBasecamp::addStandaloneQmlSource(c, "my_ui=").ok);
        QVERIFY(!LogosBasecamp::addStandaloneQmlSource(c, "=" + tmp.path()).ok);
        QVERIFY(!LogosBasecamp::addStandaloneQmlSource(c, "my_ui=" + tmp.path() + "/missing").ok);
    }
};

QTEST_GUILESS_MAIN(StandaloneModeTest)
#include "standalone_mode_test.moc"
