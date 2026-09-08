// srcdeps: ShellIntents.cpp IntentRegistry.cpp
//
// The shell's own capability list, asserted against the SAME registration the
// app performs — ShellIntents::registerWith is what MainUIBackend calls, not a
// re-implementation of it.
//
// The navigation list is the part with security consequences rather than merely
// behavioural ones: it is dispatched WITHOUT a chooser (the broker skips it when
// the shell is the only provider), so an entry that mutated state would be an
// unconsented action reachable by anything that can raise an intent.

#include "ShellIntents.h"
#include "IntentRegistry.h"
#include "LogosIntent.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

class TestShellIntents : public QObject
{
    Q_OBJECT

private slots:
    void testNavigationIntentsAreProvidedAndHandoff();
    void testNavigationIntentsAreNavigationOnly();
    void testWebReachableIntentsAreASafeSubset();
    void testEveryShellIntentIsAValidReservedName();
    void testAppLaunchTakesTheAppNameAsAParameter();
    void testPreRenameNamesAreGone();
    void testAppsCannotClaimTheNewNavigationIntents();

private:
    static IntentRegistry* freshRegistry(QObject* parent)
    {
        auto* registry = new IntentRegistry(parent);
        ShellIntents::registerWith(registry, QStringLiteral("main_ui"),
                                   QStringLiteral("Logos"), QStringLiteral("qrc:/x.svg"));
        return registry;
    }
};

void TestShellIntents::testNavigationIntentsAreProvidedAndHandoff()
{
    QObject owner;
    IntentRegistry* registry = freshRegistry(&owner);

    // Every navigation intent resolves to the shell, and every one is a
    // hand-off: `ok` means "you are there", so returning the user would undo
    // the request they made.
    for (const QString& intent : ShellIntents::kNavigationIntents) {
        const auto resolution = registry->resolve(intent);
        QVERIFY2(resolution.status == IntentRegistry::Ok,
                 qPrintable(QStringLiteral("not resolved: %1").arg(intent)));
        QCOMPARE(resolution.found.first().moduleName, QStringLiteral("main_ui"));
        QVERIFY2(registry->isHandoff(QStringLiteral("main_ui"), intent),
                 qPrintable(QStringLiteral("not a handoff: %1").arg(intent)));
    }

    // The two added with the namespace split, named explicitly so deleting one
    // fails here rather than silently shrinking the list above.
    QVERIFY(ShellIntents::kNavigationIntents.contains(
        QStringLiteral("basecamp.settings.open")));
    QVERIFY(ShellIntents::kNavigationIntents.contains(
        QStringLiteral("basecamp.apps.open")));
    QVERIFY(ShellIntents::kNavigationIntents.contains(
        QStringLiteral("basecamp.packages.open")));
}

void TestShellIntents::testNavigationIntentsAreNavigationOnly()
{
    // The list is dispatched with no consent dialog, so it must never come to
    // contain something that acts. A confirm intent appearing here would mean a
    // package could be removed without the user being asked.
    for (const QString& intent : ShellIntents::kNavigationIntents) {
        QVERIFY2(!ShellIntents::kPackageConfirmIntents.contains(intent),
                 qPrintable(QStringLiteral("%1 is a confirm intent and must not "
                                           "be chooser-exempt").arg(intent)));
        QVERIFY2(!ShellIntents::kRestrictedToPackageManagerUi.contains(intent),
                 qPrintable(QStringLiteral("%1 is restricted and must not be "
                                           "chooser-exempt").arg(intent)));
    }
}

void TestShellIntents::testWebReachableIntentsAreASafeSubset()
{
    QObject owner;
    IntentRegistry* registry = freshRegistry(&owner);

    // kWebReachableIntents used to be `= kNavigationIntents`. It is now spelled
    // out, which removes the silent-publication footgun but introduces the
    // opposite one: two lists that can drift. These assertions are what the
    // alias used to give for free.
    //
    // This list is dispatched to a web page's URL with NO consent dialog — the
    // broker skips the chooser when the shell is the provider — so it is the
    // entire boundary, and every property below is load-bearing.
    for (const QString& intent : ShellIntents::kWebReachableIntents) {
        // Nothing that acts. A confirm intent here would let a page install or
        // remove a package with nobody asked.
        QVERIFY2(!ShellIntents::kPackageConfirmIntents.contains(intent),
                 qPrintable(QStringLiteral("%1 is a confirm intent and must not "
                                           "be web-reachable").arg(intent)));
        QVERIFY2(!ShellIntents::kRestrictedToPackageManagerUi.contains(intent),
                 qPrintable(QStringLiteral("%1 is restricted and must not be "
                                           "web-reachable").arg(intent)));

        // Navigation, and only navigation. Web-reachable is a SUBSET of
        // navigation, never a superset: anything outside that list has not been
        // through the "does this merely move the user?" review.
        QVERIFY2(ShellIntents::kNavigationIntents.contains(intent),
                 qPrintable(QStringLiteral("%1 is web-reachable but is not a "
                                           "navigation intent").arg(intent)));

        // Dead config otherwise — a name nothing provides is a link that always
        // fails, which reads identically to the feature being broken.
        QVERIFY2(registry->declaresProvide(QStringLiteral("main_ui"), intent),
                 qPrintable(QStringLiteral("%1 is web-reachable but the shell "
                                           "does not provide it").arg(intent)));
    }
}

void TestShellIntents::testEveryShellIntentIsAValidReservedName()
{
    // A name that fails the grammar is dropped by registerShellProvider with
    // only a diagnostic, so a typo would silently unregister a capability.
    QStringList all = ShellIntents::kNavigationIntents;
    all += ShellIntents::kPackageConfirmIntents;
    all += ShellIntents::kWebReachableIntents;

    for (const QString& intent : all) {
        QVERIFY2(logos::intent::isValidName(intent), qPrintable(intent));
        QVERIFY2(intent.startsWith(QStringLiteral("basecamp.")),
                 qPrintable(QStringLiteral("%1 is not in the shell's reserved "
                                           "namespace").arg(intent)));
    }
}

void TestShellIntents::testAppLaunchTakesTheAppNameAsAParameter()
{
    QObject owner;
    IntentRegistry* registry = freshRegistry(&owner);

    // One name, a parameter for the target. The rejected alternative was
    // `basecamp.launch.<appName>`; this asserts the shape did not drift back.
    QVERIFY(ShellIntents::kNavigationIntents.contains(ShellIntents::kAppLaunchIntent));
    QCOMPARE(ShellIntents::kAppLaunchIntent, QStringLiteral("basecamp.apps.launch"));
    QCOMPARE(registry->resolve(ShellIntents::kAppLaunchIntent).status,
             IntentRegistry::Ok);
    QVERIFY(registry->isHandoff(QStringLiteral("main_ui"),
                                ShellIntents::kAppLaunchIntent));

    // A per-app name would need one registered intent per installed app. If
    // any such name is ever registered, the parameterised design has been
    // abandoned and the `uses`-names-a-provider problem is back.
    for (const QString& intent : ShellIntents::kNavigationIntents) {
        QVERIFY2(!intent.startsWith(QStringLiteral("basecamp.launch.")),
                 qPrintable(QStringLiteral("per-app launch intent registered: %1")
                                .arg(intent)));
    }

    // App names are not bound by the intent-name grammar, which is the concrete
    // reason the name cannot carry them: these are all legitimate module names
    // and none of them survives as an intent segment.
    for (const QString& appName : { QStringLiteral("My-App"),
                                    QStringLiteral("Wallet"),
                                    QStringLiteral("3d_viewer") }) {
        QVERIFY2(!logos::intent::isValidName(
                     QStringLiteral("basecamp.launch.") + appName),
                 qPrintable(appName));
    }
}

void TestShellIntents::testPreRenameNamesAreGone()
{
    QObject owner;
    IntentRegistry* registry = freshRegistry(&owner);

    // No alias is carried, deliberately: package_manager_ui was never released
    // declaring these, so nothing installed asks for them. A compatibility
    // window with nothing on the far side is a second live path to a restricted
    // capability and nothing else — the pre-rename `confirm_uninstall` in
    // particular would be an unrestricted route to the restricted one, since
    // restrictions are keyed on the name as submitted.
    for (const QString& old : { QStringLiteral("logos.repositories.manage"),
                                QStringLiteral("logos.packages.confirm_install"),
                                QStringLiteral("logos.packages.confirm_uninstall"),
                                QStringLiteral("logos.packages.confirm_upgrade") }) {
        QVERIFY2(registry->resolve(old).status == IntentRegistry::None,
                 qPrintable(QStringLiteral("pre-rename name still resolves: %1")
                                .arg(old)));
        QVERIFY2(!registry->declaresProvide(QStringLiteral("main_ui"), old),
                 qPrintable(old));
    }
}

void TestShellIntents::testAppsCannotClaimTheNewNavigationIntents()
{
    // The new names are in the reserved namespace, so an installed app
    // declaring one is refused — otherwise it could intercept a navigation the
    // shell dispatches without asking the user.
    QTemporaryDir root;
    const QString dir = root.filePath(QStringLiteral("squatter"));
    QVERIFY(QDir().mkpath(dir));
    QFile meta(QDir(dir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(meta.open(QIODevice::WriteOnly));
    meta.write(R"({"provides":[{"intent":"basecamp.settings.open"},
                                {"intent":"basecamp.apps.open"}]})");
    meta.close();

    QObject owner;
    IntentRegistry* registry = freshRegistry(&owner);
    registry->rebuild({ { QStringLiteral("squatter_ui"),
                          QVariantMap{ { QStringLiteral("installDir"), dir },
                                       { QStringLiteral("type"),
                                         QStringLiteral("ui_qml") } } } },
                      nullptr, nullptr);

    for (const QString& intent : { QStringLiteral("basecamp.settings.open"),
                                   QStringLiteral("basecamp.apps.open") }) {
        QVERIFY(!registry->declaresProvide(QStringLiteral("squatter_ui"), intent));
        // Still the shell's, and only the shell's.
        const auto resolution = registry->resolve(intent);
        QCOMPARE(resolution.status, IntentRegistry::Ok);
        QCOMPARE(resolution.found.size(), 1);
        QCOMPARE(resolution.found.first().moduleName, QStringLiteral("main_ui"));
    }
}

QTEST_MAIN(TestShellIntents)
#include "shell_intents_test.moc"
