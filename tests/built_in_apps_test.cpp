// srcdeps: utils/BuiltInApps.cpp utils/InstalledPlacement.cpp
//
// Unit tests for Basecamp's own apps (app/utils/BuiltInApps.{h,cpp}): which
// copies the scans report from where the user installs, moving them aside,
// and what the user is told. The scans are faked: what the core and
// package_manager report is all that is read.

#include "../app/utils/BuiltInApps.h"
#include "../app/interfaces/ICoreRuntime.h"

#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace logos::builtin;

namespace {

void touch(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
}

QVariantMap listed(const QString& name, const QString& installType, const QString& installDir)
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("installType"), installType},
            {QStringLiteral("installDir"), installDir}};
}

// The modules info the core reports, and a rescan that, like the core's,
// forgets what is no longer on disk.
class FakeRuntime : public ICoreRuntime {
public:
    QJsonArray modules;
    int rescans = 0;
    // Each unload: the name, with its dependents, and whether the file it was
    // loaded from was still where the core had it.
    QStringList unloads;

    void add(const QString& name, const QString& path, bool loaded = false)
    {
        modules.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("path"), path},
                                   {QStringLiteral("loaded"), loaded}});
    }
    bool unloadModule(const QString& name, bool withDependents) override
    {
        for (int i = 0; i < modules.size(); ++i) {
            QJsonObject m = modules[i].toObject();
            if (m.value(QStringLiteral("name")).toString() != name)
                continue;
            const bool inPlace = QFileInfo::exists(m.value(QStringLiteral("path")).toString());
            unloads << QStringLiteral("%1 dependents=%2 inPlace=%3")
                           .arg(name).arg(int(withDependents)).arg(int(inPlace));
            m[QStringLiteral("loaded")] = false;
            modules[i] = m;
        }
        return true;
    }

    QString modulesInfoJson() const override
    {
        return QString::fromUtf8(QJsonDocument(modules).toJson(QJsonDocument::Compact));
    }
    void refreshModules() override
    {
        ++rescans;
        QJsonArray kept;
        for (const QJsonValue& m : modules)
            if (QFileInfo::exists(m.toObject().value(QStringLiteral("path")).toString()))
                kept.append(m);
        modules = kept;
    }
    void start() override {}
    QStringList knownModules() const override { return {}; }
    QStringList loadedModules() const override { return {}; }
    bool loadModule(const QString&, LoadPolicy) override { return true; }
    QVariantList allStats() const override { return {}; }
};

} // namespace

class BuiltInAppsTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tmp;
    int serial = 0;
    QString user;    // where the user installs
    QString bundle;  // Basecamp's own
    QString aside;   // where copies go

private slots:
    void init()
    {
        ++serial;
        user = QDir::cleanPath(tmp.filePath(QStringLiteral("user%1").arg(serial)));
        bundle = QDir::cleanPath(tmp.filePath(QStringLiteral("bundle%1").arg(serial)));
        aside = QDir::cleanPath(tmp.filePath(QStringLiteral("aside%1").arg(serial)));
        QVERIFY(QDir().mkpath(user));
        QVERIFY(QDir().mkpath(bundle));
    }

    void theNamesAreTheAppsAndTheirBackends()
    {
        QVERIFY(isBuiltIn(QStringLiteral("package_manager_ui")));
        QVERIFY(isBuiltIn(QStringLiteral("package_manager_ui_backend")));
        QVERIFY(isBuiltIn(QStringLiteral("Package_Manager_UI")));
        QVERIFY(!isBuiltIn(QStringLiteral("package_manager")));
        QVERIFY(!isBuiltIn(QStringLiteral("package_manager_ui_backend_backend")));
        QVERIFY(!isBuiltIn(QStringLiteral("wallet_ui")));
    }

    // The core's own view decides: a built-in name it knows from under the
    // user's dir, by its package dir, or the file when it sits loose.
    void theCoreReportsCopiesFromTheUsersDir()
    {
        FakeRuntime core;
        core.add(QStringLiteral("package_manager_ui_backend"), user + QStringLiteral("/pmb/b.so"));
        core.add(QStringLiteral("Package_Manager_UI"), user + QStringLiteral("/odd/x.so"));
        core.add(QStringLiteral("package_manager_ui"), user + QStringLiteral("/loose.so"));
        core.add(QStringLiteral("package_manager_ui_backend"),
                 bundle + QStringLiteral("/plugins/package_manager_ui/b.so"));
        core.add(QStringLiteral("wallet_ui_backend"), user + QStringLiteral("/wallet/w.so"));

        const QList<SetAside> copies = copiesInCore(core.modulesInfoJson(), user);
        QCOMPARE(copies.size(), 3);
        QCOMPARE(copies[0].from, user + QStringLiteral("/pmb"));
        QCOMPARE(copies[1].from, user + QStringLiteral("/odd"));
        QCOMPARE(copies[2].from, user + QStringLiteral("/loose.so"));
        for (const SetAside& copy : copies)
            QVERIFY(copy.movable && copy.to.isEmpty());

        // No user dir (standalone mode): nothing is the user's.
        QVERIFY(copiesInCore(core.modulesInfoJson(), QString()).isEmpty());
    }

    // package_manager's list decides for apps: whatever is not Basecamp's
    // own, movable only when it sits directly in the user's dir.
    void packageManagerReportsAppCopies()
    {
        const QVariantList apps{
            listed(QStringLiteral("package_manager_ui"), QStringLiteral("embedded"),
                   bundle + QStringLiteral("/plugins/package_manager_ui")),
            listed(QStringLiteral("package_manager_ui"), QStringLiteral("user"),
                   user + QStringLiteral("/package_manager_ui")),
            listed(QStringLiteral("package_manager_ui_backend"), QStringLiteral("user"),
                   user + QStringLiteral("/nested/deeper")),
            listed(QStringLiteral("package_manager_ui"), QStringLiteral("user"),
                   tmp.filePath(QStringLiteral("elsewhere/package_manager_ui"))),
            listed(QStringLiteral("wallet_ui"), QStringLiteral("user"), user + QStringLiteral("/wallet_ui")),
        };
        const QList<SetAside> copies = copiesInApps(apps, user);
        QCOMPARE(copies.size(), 3);
        QCOMPARE(copies[0].from, user + QStringLiteral("/package_manager_ui"));
        QVERIFY(copies[0].movable);
        QVERIFY(!copies[1].movable);
        QVERIFY(!copies[2].movable);
    }

    // Moved whole into the set-aside dir, under a name of its own each time;
    // one that may not move, or is gone, stays where it is.
    void aCopyMovesAside()
    {
        const QString pmui = user + QStringLiteral("/package_manager_ui");
        touch(pmui + QStringLiteral("/manifest.json"));
        const SetAside moved = setAside({QStringLiteral("package_manager_ui"), pmui}, aside);
        QVERIFY(moved.to.startsWith(aside + QStringLiteral("/package_manager_ui-")));
        QVERIFY(!QFileInfo::exists(pmui));
        QVERIFY(QFileInfo::exists(moved.to + QStringLiteral("/manifest.json")));

        touch(pmui + QStringLiteral("/manifest.json"));
        const SetAside again = setAside({QStringLiteral("package_manager_ui"), pmui}, aside);
        QVERIFY(!again.to.isEmpty());
        QVERIFY(again.to != moved.to);
        QVERIFY(QFileInfo::exists(again.to + QStringLiteral("/manifest.json")));
        QVERIFY(QFileInfo::exists(moved.to + QStringLiteral("/manifest.json")));

        const QString stuckDir = user + QStringLiteral("/stuck");
        touch(stuckDir + QStringLiteral("/manifest.json"));
        SetAside stuck{QStringLiteral("package_manager_ui"), stuckDir};
        stuck.movable = false;
        QVERIFY(setAside(stuck, aside).to.isEmpty());
        QVERIFY(QFileInfo::exists(stuckDir + QStringLiteral("/manifest.json")));

        QVERIFY(setAside({QStringLiteral("package_manager_ui"), user + QStringLiteral("/gone")}, aside)
                    .to.isEmpty());
    }

    // Every copy the core found moves, then one rescan so the core forgets
    // them; with nothing found, no rescan.
    void theCoreForgetsWhatMoved()
    {
        touch(user + QStringLiteral("/pmb/b.so"));
        touch(user + QStringLiteral("/wallet/w.so"));
        FakeRuntime core;
        core.add(QStringLiteral("package_manager_ui_backend"), user + QStringLiteral("/pmb/b.so"));
        core.add(QStringLiteral("wallet_ui_backend"), user + QStringLiteral("/wallet/w.so"));

        const QList<SetAside> copies = setAsideFromCore(core, user, aside);
        QCOMPARE(copies.size(), 1);
        QVERIFY(!copies[0].to.isEmpty());
        QVERIFY(!QFileInfo::exists(user + QStringLiteral("/pmb")));
        QCOMPARE(core.rescans, 1);
        QVERIFY(copiesInCore(core.modulesInfoJson(), user).isEmpty());

        QVERIFY(setAsideFromCore(core, user, aside).isEmpty());
        QCOMPARE(core.rescans, 1);
        QVERIFY(core.unloads.isEmpty());
    }

    // A rescan repoints a loaded module's entry at the copy it found, so what
    // runs under that name may be the copy or Basecamp's own: it is stopped,
    // with what depends on it, while the copy is still in place, then moved.
    void aLoadedCopyIsStoppedBeforeItMoves()
    {
        touch(user + QStringLiteral("/pmb/b.so"));
        FakeRuntime core;
        core.add(QStringLiteral("package_manager_ui_backend"), user + QStringLiteral("/pmb/b.so"),
                 /*loaded=*/true);

        const QList<SetAside> copies = setAsideFromCore(core, user, aside);
        QCOMPARE(core.unloads,
                 QStringList{QStringLiteral("package_manager_ui_backend dependents=1 inPlace=1")});
        QCOMPARE(copies.size(), 1);
        QVERIFY(copies[0].loaded);
        QVERIFY(!copies[0].to.isEmpty());
        QVERIFY(!QFileInfo::exists(user + QStringLiteral("/pmb")));
        QVERIFY2(setAsideMessage(copies[0]).contains(QStringLiteral("stopped what was running")),
                 qPrintable(setAsideMessage(copies[0])));
    }

    // What the user reads names the app and where its copy is.
    void theNoticeSaysWhereTheCopyIs()
    {
        const SetAside moved{QStringLiteral("package_manager_ui"), user + QStringLiteral("/package_manager_ui"),
                             aside + QStringLiteral("/package_manager_ui-1")};
        QCOMPARE(setAsideTitle(moved), QStringLiteral("Built-in App Copy Moved"));
        QVERIFY(setAsideMessage(moved).contains(QStringLiteral("package_manager_ui")));
        QVERIFY(setAsideMessage(moved).contains(QDir::toNativeSeparators(moved.from)));
        QVERIFY(setAsideMessage(moved).contains(QDir::toNativeSeparators(moved.to)));

        const SetAside left{QStringLiteral("package_manager_ui"), user + QStringLiteral("/package_manager_ui")};
        QCOMPARE(setAsideTitle(left), QStringLiteral("Built-in App Copy Not Used"));
        QVERIFY(setAsideMessage(left).contains(QStringLiteral("could not be moved")));
    }
};

QTEST_GUILESS_MAIN(BuiltInAppsTest)
#include "built_in_apps_test.moc"
