// srcdeps: utils/InstalledPlacement.cpp
//
// Unit tests for the launch-time integrity check's placement and pre-flight
// (app/utils/InstalledPlacement.{h,cpp}): which plugin paths count as installed
// by the package manager, and which modules a load would bring up that must be
// checked first. The check itself (hashing, via liblgx) is passed in as a
// fake here; tests/ui-tests.mjs drives the real one against a tampered app.

#include "../app/utils/InstalledPlacement.h"
#include "../app/interfaces/ICoreRuntime.h"

#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>

using namespace logos::integrity;

namespace {

void touch(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
}

// Just enough of a runtime: the modules info the pre-flight walks.
class FakeRuntime : public ICoreRuntime {
public:
    QJsonArray modules;

    void add(const QString& name, const QString& path, const QStringList& deps = {},
             const QStringList& optional = {}, bool loaded = false)
    {
        modules.append(QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("path"), path},
            {QStringLiteral("loaded"), loaded},
            {QStringLiteral("dependencies"), QJsonArray::fromStringList(deps)},
            {QStringLiteral("optional_dependencies"), QJsonArray::fromStringList(optional)},
        });
    }

    QString modulesInfoJson() const override
    {
        return QString::fromUtf8(QJsonDocument(modules).toJson(QJsonDocument::Compact));
    }
    void start() override {}
    QStringList knownModules() const override { return {}; }
    QStringList loadedModules() const override { return {}; }
    bool loadModule(const QString&, LoadPolicy) override { return true; }
    bool unloadModule(const QString&, bool) override { return true; }
    void refreshModules() override {}
    QVariantList allStats() const override { return {}; }
};

} // namespace

class InstalledPlacementTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tmp;
    QString root;  // the user modules dir

private slots:
    void init()
    {
        QVERIFY(tmp.isValid());
        root = tmp.filePath(QStringLiteral("modules"));
        QDir().mkpath(root);
    }

    void aPluginInAPackageIsInside()
    {
        touch(root + QStringLiteral("/app/lib/app_plugin.so"));
        const Placement p = placementUnder(root + QStringLiteral("/app/lib/app_plugin.so"), root);
        QVERIFY(p.inside);
        QCOMPARE(p.packageDir, QDir::cleanPath(root + QStringLiteral("/app")));
    }

    void aPluginElsewhereIsOutside()
    {
        touch(tmp.filePath(QStringLiteral("dev_build/app/app_plugin.so")));
        QVERIFY(!placementUnder(tmp.filePath(QStringLiteral("dev_build/app/app_plugin.so")), root).inside);
        QVERIFY(!placementUnder(tmp.filePath(QStringLiteral("modules-other/app/x.so")), root).inside);
    }

    // Nothing lgpm installs sits loose in the root: inside, but no package.
    void aPluginLooseInTheRootHasNoPackage()
    {
        touch(root + QStringLiteral("/loose_plugin.so"));
        const Placement p = placementUnder(root + QStringLiteral("/loose_plugin.so"), root);
        QVERIFY(p.inside);
        QVERIFY(p.packageDir.isEmpty());
        QVERIFY(!problemWithPlugin(root + QStringLiteral("/loose_plugin.so"), root,
                                   [](const QString&) { return QString(); }).isEmpty());
    }

    void noRootChecksNothing()
    {
        QVERIFY(!placementUnder(root + QStringLiteral("/app/a.so"), QString()).inside);
    }

    // A package dir that is itself a symlink stays inside: resolving it would
    // carry it out of the root and past the check.
    void aSymlinkedPackageDirIsInside()
    {
        touch(tmp.filePath(QStringLiteral("elsewhere/linked/linked_plugin.so")));
        QVERIFY(QFile::link(tmp.filePath(QStringLiteral("elsewhere/linked")),
                            root + QStringLiteral("/linked")));
        const Placement p = placementUnder(root + QStringLiteral("/linked/linked_plugin.so"), root);
        QVERIFY(p.inside);
        QCOMPARE(p.packageDir, QDir::cleanPath(root + QStringLiteral("/linked")));
    }

    // `link/..` climbs out of the link's TARGET: judged on the text alone the
    // plugin would look outside, while the file opened is the installed one.
    void dotDotThroughASymlinkIsInside()
    {
        const QString realRoot = tmp.filePath(QStringLiteral("real/modules"));
        touch(realRoot + QStringLiteral("/far/far_plugin.so"));
        QDir().mkpath(tmp.filePath(QStringLiteral("real/sub")));
        QVERIFY(QFile::link(tmp.filePath(QStringLiteral("real/sub")), tmp.filePath(QStringLiteral("link"))));

        const QString viaLink = tmp.filePath(QStringLiteral("link")) + QStringLiteral("/../modules/far/far_plugin.so");
        const Placement p = placementUnder(viaLink, realRoot);
        QVERIFY(p.inside);
        QVERIFY(p.packageDir.endsWith(QStringLiteral("/modules/far")));
    }

    // A root reached through a symlink is the same root.
    void aRootReachedThroughASymlinkMatches()
    {
        touch(root + QStringLiteral("/app/app_plugin.so"));
        QVERIFY(QFile::link(root, tmp.filePath(QStringLiteral("modules-link"))));
        QVERIFY(placementUnder(root + QStringLiteral("/app/app_plugin.so"),
                               tmp.filePath(QStringLiteral("modules-link"))).inside);
    }

    // Everything the load would bring up is checked -- required and optional,
    // transitively -- and a failure names its module.
    void thePreflightWalksRequiredAndOptionalDependencies()
    {
        FakeRuntime core;
        core.add(QStringLiteral("app"), root + QStringLiteral("/app/app.so"),
                 {QStringLiteral("base")}, {QStringLiteral("extra")});
        core.add(QStringLiteral("base"), root + QStringLiteral("/base/base.so"));
        core.add(QStringLiteral("extra"), root + QStringLiteral("/extra/extra.so"),
                 {QStringLiteral("deep")});
        core.add(QStringLiteral("deep"), root + QStringLiteral("/deep/deep.so"));

        QStringList checked;
        const Preflight r = preflight(core, QStringLiteral("app"), root,
            [&](const QString& dir) {
                checked << QFileInfo(dir).fileName();
                return dir.endsWith(QStringLiteral("/deep")) ? QStringLiteral("bad") : QString();
            });
        // `deep` is only reached through the optional `extra`: the module
        // loads, without its optional dependencies.
        QCOMPARE(r.refusal, QString());
        QCOMPARE(r.withoutOptional, QStringLiteral("deep: bad"));
        QCOMPARE(checked, (QStringList{QStringLiteral("app"), QStringLiteral("base"),
                                       QStringLiteral("extra"), QStringLiteral("deep")}));
    }

    // The core keeps adding optional dependencies until nothing new appears,
    // so the pre-flight must follow them as far: an optional dependency of an
    // optional one, and one behind a required edge in the optional part.
    void optionalDependenciesAreCheckedAtAnyDepth()
    {
        const auto deepIsBad = [](const QString& dir) {
            return dir.endsWith(QStringLiteral("/deep")) ? QStringLiteral("bad") : QString();
        };

        FakeRuntime chain;
        chain.add(QStringLiteral("app"), root + QStringLiteral("/app/app.so"), {}, {QStringLiteral("extra")});
        chain.add(QStringLiteral("extra"), root + QStringLiteral("/extra/extra.so"), {}, {QStringLiteral("deep")});
        chain.add(QStringLiteral("deep"), root + QStringLiteral("/deep/deep.so"));
        Preflight r = preflight(chain, QStringLiteral("app"), root, deepIsBad);
        QCOMPARE(r.refusal, QString());
        QCOMPARE(r.withoutOptional, QStringLiteral("deep: bad"));

        FakeRuntime viaRequired;
        viaRequired.add(QStringLiteral("app"), root + QStringLiteral("/app/app.so"), {}, {QStringLiteral("extra")});
        viaRequired.add(QStringLiteral("extra"), root + QStringLiteral("/extra/extra.so"), {QStringLiteral("mid")});
        viaRequired.add(QStringLiteral("mid"), root + QStringLiteral("/mid/mid.so"), {}, {QStringLiteral("deep")});
        viaRequired.add(QStringLiteral("deep"), root + QStringLiteral("/deep/deep.so"));
        r = preflight(viaRequired, QStringLiteral("app"), root, deepIsBad);
        QCOMPARE(r.refusal, QString());
        QCOMPARE(r.withoutOptional, QStringLiteral("deep: bad"));

        FakeRuntime longChain;
        longChain.add(QStringLiteral("app"), root + QStringLiteral("/app/app.so"), {}, {QStringLiteral("a")});
        longChain.add(QStringLiteral("a"), root + QStringLiteral("/a/a.so"), {}, {QStringLiteral("b")});
        longChain.add(QStringLiteral("b"), root + QStringLiteral("/b/b.so"), {}, {QStringLiteral("c")});
        longChain.add(QStringLiteral("c"), root + QStringLiteral("/c/c.so"), {}, {QStringLiteral("deep")});
        longChain.add(QStringLiteral("deep"), root + QStringLiteral("/deep/deep.so"));
        r = preflight(longChain, QStringLiteral("app"), root, deepIsBad);
        QCOMPARE(r.refusal, QString());
        QCOMPARE(r.withoutOptional, QStringLiteral("deep: bad"));
    }

    void anOptionalCycleTerminates()
    {
        FakeRuntime core;
        core.add(QStringLiteral("app"), root + QStringLiteral("/app/app.so"), {}, {QStringLiteral("a")});
        core.add(QStringLiteral("a"), root + QStringLiteral("/a/a.so"), {}, {QStringLiteral("b")});
        core.add(QStringLiteral("b"), root + QStringLiteral("/b/b.so"), {}, {QStringLiteral("a"), QStringLiteral("app")});
        int calls = 0;
        const Preflight r = preflight(core, QStringLiteral("app"), root,
            [&](const QString&) { ++calls; return QString(); });
        QCOMPARE(r.withoutOptional, QString());
        QCOMPARE(calls, 3);
    }

    // Already running: not reloaded by this call, so not checked. Bundled or
    // developer modules are outside the root and never checked.
    void runningAndOutsideModulesAreNotChecked()
    {
        FakeRuntime core;
        core.add(QStringLiteral("app"), root + QStringLiteral("/app/app.so"),
                 {QStringLiteral("running"), QStringLiteral("bundled")});
        core.add(QStringLiteral("running"), root + QStringLiteral("/running/r.so"), {}, {}, true);
        core.add(QStringLiteral("bundled"), tmp.filePath(QStringLiteral("bundle/modules/b/b.so")));

        QStringList checked;
        QVERIFY(preflight(core, QStringLiteral("app"), root,
            [&](const QString& dir) { checked << QFileInfo(dir).fileName(); return QString(); })
                    .refusal.isEmpty());
        QCOMPARE(checked, QStringList{QStringLiteral("app")});
    }

    // A required dependency failing refuses the module outright.
    void aRequiredFailureRefuses()
    {
        FakeRuntime core;
        core.add(QStringLiteral("app"), root + QStringLiteral("/app/app.so"),
                 {QStringLiteral("base")}, {QStringLiteral("extra")});
        core.add(QStringLiteral("base"), root + QStringLiteral("/base/base.so"));
        core.add(QStringLiteral("extra"), root + QStringLiteral("/extra/extra.so"));
        const Preflight r = preflight(core, QStringLiteral("app"), root,
            [](const QString& dir) {
                return dir.endsWith(QStringLiteral("/base")) ? QStringLiteral("bad") : QString();
            });
        QCOMPARE(r.refusal, QStringLiteral("base: bad"));
        QCOMPARE(r.culprit, QStringLiteral("base"));
        QCOMPARE(r.culpritDir, QDir::cleanPath(root + QStringLiteral("/base")));
    }

    // A module both required (by one module) and optional (to another) is
    // required: its failure refuses.
    void aModuleRequiredAnywhereIsRequired()
    {
        FakeRuntime core;
        core.add(QStringLiteral("app"), root + QStringLiteral("/app/app.so"),
                 {QStringLiteral("base")}, {QStringLiteral("shared")});
        core.add(QStringLiteral("base"), root + QStringLiteral("/base/base.so"), {QStringLiteral("shared")});
        core.add(QStringLiteral("shared"), root + QStringLiteral("/shared/shared.so"));
        const Preflight r = preflight(core, QStringLiteral("app"), root,
            [](const QString& dir) {
                return dir.endsWith(QStringLiteral("/shared")) ? QStringLiteral("bad") : QString();
            });
        QCOMPARE(r.refusal, QStringLiteral("shared: bad"));
    }

    // Module info the runtime cannot give refuses rather than checks nothing.
    void unreadableModuleInfoRefuses()
    {
        struct BrokenRuntime final : FakeRuntime {
            QString modulesInfoJson() const override { return QString(); }
        } core;
        const Preflight r = preflight(core, QStringLiteral("app"), root,
                                      [](const QString&) { return QString(); });
        QVERIFY(!r.refusal.isEmpty());
    }

    void aDependencyCycleTerminates()
    {
        FakeRuntime core;
        core.add(QStringLiteral("a"), root + QStringLiteral("/a/a.so"), {QStringLiteral("b")});
        core.add(QStringLiteral("b"), root + QStringLiteral("/b/b.so"), {QStringLiteral("a")});
        int calls = 0;
        QVERIFY(preflight(core, QStringLiteral("a"), root,
            [&](const QString&) { ++calls; return QString(); }).refusal.isEmpty());
        QCOMPARE(calls, 2);
    }

    // A UI app is checked by where its files are, not by what its metadata
    // says: anything it opens in the installed apps dir has its package
    // checked, once, and a file loose in that dir is refused.
    void anAppIsCheckedByWhereItsFilesAre()
    {
        const QStringList opened{root + QStringLiteral("/app/qml/Main.qml"),
                                 root + QStringLiteral("/app/lib/app.so"),
                                 root + QStringLiteral("/app/manifest.json"),
                                 tmp.filePath(QStringLiteral("bundled/x/x.so"))};
        QStringList checked;
        QVERIFY(problemWithAny(opened, root,
                               [&](const QString& dir) { checked << dir; return QString(); }).isEmpty());
        QCOMPARE(checked, QStringList{QDir::cleanPath(root + QStringLiteral("/app"))});

        QString failedDir;
        const Problem p = problemWithAny(opened, root,
                                         [](const QString&) { return QStringLiteral("bad"); }, &failedDir);
        QCOMPARE(p.what, QStringLiteral("bad"));
        QCOMPARE(failedDir, QDir::cleanPath(root + QStringLiteral("/app")));

        int calls = 0;
        QVERIFY(problemWithAny({tmp.filePath(QStringLiteral("bundled/x/x.so")), QString()}, root,
                               [&](const QString&) { ++calls; return QStringLiteral("bad"); }).isEmpty());
        QCOMPARE(calls, 0);

        const Problem loose = problemWithAny({root + QStringLiteral("/stray.qml")}, root,
                                             [](const QString&) { return QString(); });
        QVERIFY(!loose.isEmpty());
        QCOMPARE(loose.kind, Problem::Kind::Unverifiable);
    }

    // What went wrong decides the headline and the advice, and a failing
    // dependency is named rather than blamed on the app.
    void refusalsSayWhatWentWrong()
    {
        using Kind = Problem::Kind;
        QString m = refusalMessage(QStringLiteral("app"), QStringLiteral("app"), {},
                                   Problem(QStringLiteral("its files were modified"), Kind::Changed));
        QVERIFY2(m.contains(QStringLiteral("its files changed after it was installed")), qPrintable(m));
        QVERIFY(m.contains(QStringLiteral("Reinstall it in Package Manager.")));
        QVERIFY(m.endsWith(QStringLiteral("app: its files were modified")));

        m = refusalMessage(QStringLiteral("app"), QStringLiteral("depsvc"), {},
                           Problem(QStringLiteral("its files were modified"), Kind::Changed));
        QVERIFY2(m.contains(QStringLiteral("a module it needs, depsvc, changed after it was installed")),
                 qPrintable(m));
        QVERIFY(m.contains(QStringLiteral("Reinstall depsvc in Package Manager.")));
        QVERIFY(!m.contains(QStringLiteral("its files changed")));

        m = refusalMessage(QStringLiteral("app"), QStringLiteral("app"), {},
                           Problem(QStringLiteral("its manifest carries no content hashes"), Kind::Unverifiable));
        QVERIFY2(m.contains(QStringLiteral("cannot be verified")), qPrintable(m));
        QVERIFY(!m.contains(QStringLiteral("changed")));

        m = refusalMessage(QStringLiteral("app"), QStringLiteral("app"), {},
                           Problem(QStringLiteral("it could not be checked: I/O error"), Kind::Unchecked));
        QVERIFY2(m.contains(QStringLiteral("could not be checked")), qPrintable(m));
        QVERIFY(m.contains(QStringLiteral("Try again.")));
    }

    // Package Manager is no way to repair Package Manager.
    void packageManagerIsNotSentToItselfForRepair()
    {
        const QString dir = root + QStringLiteral("/package_manager");
        const QString m = refusalMessage(QStringLiteral("package_manager_ui"), QStringLiteral("package_manager"),
                                         dir, Problem(QStringLiteral("its files were modified")));
        QVERIFY2(m.contains(QStringLiteral("Package Manager cannot repair package_manager")), qPrintable(m));
        QVERIFY(m.contains(QDir::toNativeSeparators(dir)));
        QVERIFY(!m.contains(QStringLiteral("Reinstall")));
    }

    void theStartupNoticeNamesTheModuleThatKeptTheInstalledModulesOut()
    {
        const QString m = installedModulesSkippedMessage(
            QStringLiteral("capability_module"), Problem(QStringLiteral("its files were modified")));
        QVERIFY2(m.contains(QStringLiteral("started without the modules you installed")), qPrintable(m));
        QVERIFY(m.contains(QStringLiteral("Reinstall capability_module")));
        QVERIFY(m.contains(QStringLiteral("capability_module: its files were modified")));
    }

    void startupNoticesAreTakenOnce()
    {
        addStartupNotice({QStringLiteral("Title"), QStringLiteral("mod"), QStringLiteral("message")});
        const auto taken = takeStartupNotices();
        QCOMPARE(taken.size(), 1);
        QCOMPARE(taken.first().module, QStringLiteral("mod"));
        QVERIFY(takeStartupNotices().isEmpty());
    }

    // Loading a core module from the UI must not hash on the UI thread: the
    // check runs on a worker, the answer comes back on the calling thread.
    void theBackgroundPreflightChecksOffTheCallingThread()
    {
        FakeRuntime core;
        core.add(QStringLiteral("mod"), root + QStringLiteral("/mod/mod.so"));
        QThread* checkedOn = nullptr;
        QThread* answeredOn = nullptr;
        Preflight answer;
        bool answered = false;
        QObject context;
        preflightInBackground(core.modulesInfoJson(), QStringLiteral("mod"), root,
            [&](const QString&) { checkedOn = QThread::currentThread(); return QStringLiteral("bad"); },
            &context,
            [&](const Preflight& r) { answeredOn = QThread::currentThread(); answer = r; answered = true; });
        QTRY_VERIFY(answered);
        QVERIFY(checkedOn != nullptr);
        QVERIFY(checkedOn != QThread::currentThread());
        QCOMPARE(answeredOn, QThread::currentThread());
        QCOMPARE(answer.refusal, QStringLiteral("mod: bad"));
    }

    void noAnswerForAContextThatIsGone()
    {
        FakeRuntime core;
        core.add(QStringLiteral("mod"), root + QStringLiteral("/mod/mod.so"));
        bool answered = false;
        std::atomic<bool> checked{false};
        {
            QObject context;
            preflightInBackground(core.modulesInfoJson(), QStringLiteral("mod"), root,
                [&](const QString&) { checked = true; return QString(); },
                &context, [&](const Preflight&) { answered = true; });
        }
        QTRY_VERIFY(checked.load());
        QTest::qWait(50);
        QVERIFY(!answered);
    }

    void withNoRootNothingIsChecked()
    {
        FakeRuntime core;
        core.add(QStringLiteral("app"), root + QStringLiteral("/app/app.so"));
        int calls = 0;
        QVERIFY(preflight(core, QStringLiteral("app"), QString(),
            [&](const QString&) { ++calls; return QStringLiteral("bad"); }).refusal.isEmpty());
        QCOMPARE(calls, 0);
    }
};

QTEST_GUILESS_MAIN(InstalledPlacementTest)
#include "installed_placement_test.moc"
