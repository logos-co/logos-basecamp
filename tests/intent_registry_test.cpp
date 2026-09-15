// srcdeps: IntentRegistry.cpp
//
// IntentRegistry resolution rules. These are the parts with security
// consequences — reserved-namespace refusal, shell-identity protection,
// deterministic ordering — so they are unit-tested against real files on disk
// rather than mocked out.

#include "IntentRegistry.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Write a metadata.json into a fresh subdirectory and return its path.
QString makeApp(QTemporaryDir& root, const QString& name, const QByteArray& json)
{
    const QString dir = root.filePath(name);
    QDir().mkpath(dir);
    QFile file(QDir(dir).filePath(QStringLiteral("metadata.json")));
    file.open(QIODevice::WriteOnly);
    file.write(json);
    file.close();
    return dir;
}

QVariantMap plugin(const QString& installDir, const QString& type = QStringLiteral("ui_qml"))
{
    return QVariantMap{
        { QStringLiteral("installDir"), installDir },
        { QStringLiteral("type"), type },
    };
}

} // namespace

class TestIntentRegistry : public QObject {
    Q_OBJECT
private slots:
    void testResolvesSingleProvider();
    void testAmbiguousIsSortedByModuleName();
    void testNoProviderIsNone();
    void testDeclaresUseAndProvide();
    void testReservedNamespaceRefusedFromApps();
    void testPlatformNamespaceRefusedFromApps();
    void testShellMayProvideReservedNamespace();
    void testDiskRecordCannotClaimShellIdentity();
    void testMalformedJsonIsDiagnosedNotFatal();
    void testBareStringArrayIsRefused();
    void testInvalidNameIsRejected();
    void testNonUiQmlIsSkipped();
    void testUsesCardinalityParsedAndDiagnosed();
    void testShellRegistrationSurvivesRebuild();
    void testProvidesCarriesTheParamShape();
    void testRestrictedIntentAllowsOnlyListedRequesters();
    void testEmptyRequesterListIsRefusedNotAnOpenDoor();
    void testRestrictionSurvivesRebuild();
    void testHandoffIsReadPerProviderIntent();
    void testNonBooleanHandoffIsDiagnosedNotCoerced();
    void testProvidersMayDisagreeAboutHandoff();
    void testShellHandoffSurvivesRebuild();
    void testInstallableNeverOverlapsAnInstalledPackage();
    void testInstallableRefusesReservedAndInvalidNames();
    void testInstallableIsSortedAndDeduplicated();
};

void TestIntentRegistry::testResolvesSingleProvider()
{
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("pm"),
        R"({"name":"package_manager_ui","provides":[{"intent":"packages.show"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("package_manager_ui"), plugin(dir) } },
                     [](const QString&) { return QStringLiteral("Package Manager"); },
                     [](const QString&) { return QStringLiteral("qrc:/pm.png"); });

    const auto resolution = registry.resolve(QStringLiteral("packages.show"));
    QCOMPARE(resolution.status, IntentRegistry::Ok);
    QCOMPARE(resolution.found.size(), 1);
    QCOMPARE(resolution.found.first().moduleName, QStringLiteral("package_manager_ui"));
    QCOMPARE(resolution.found.first().displayName, QStringLiteral("Package Manager"));
    QCOMPARE(resolution.found.first().iconSource, QStringLiteral("qrc:/pm.png"));
}

void TestIntentRegistry::testAmbiguousIsSortedByModuleName()
{
    // A chooser whose rows move between runs is confusing to a user and
    // impossible to test, so ordering is part of the contract.
    QTemporaryDir root;
    const QString zebra = makeApp(root, QStringLiteral("z"),
        R"({"provides":[{"intent":"wallet.send"}]})");
    const QString alpha = makeApp(root, QStringLiteral("a"),
        R"({"provides":[{"intent":"wallet.send"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("zebra_ui"), plugin(zebra) },
                       { QStringLiteral("alpha_ui"), plugin(alpha) } },
                     nullptr, nullptr);

    const auto resolution = registry.resolve(QStringLiteral("wallet.send"));
    QCOMPARE(resolution.status, IntentRegistry::Ambiguous);
    QCOMPARE(resolution.found.size(), 2);
    QCOMPARE(resolution.found.at(0).moduleName, QStringLiteral("alpha_ui"));
    QCOMPARE(resolution.found.at(1).moduleName, QStringLiteral("zebra_ui"));
}

void TestIntentRegistry::testNoProviderIsNone()
{
    IntentRegistry registry;
    registry.rebuild({}, nullptr, nullptr);
    QCOMPARE(registry.resolve(QStringLiteral("nobody.here")).status, IntentRegistry::None);
    QCOMPARE(registry.resolve(QString()).status, IntentRegistry::None);
}

void TestIntentRegistry::testDeclaresUseAndProvide()
{
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("chat"),
        R"({"uses":[{"intent":"packages.show","cardinality":"single"}],
            "provides":[{"intent":"chat.open"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("chat_ui"), plugin(dir) } }, nullptr, nullptr);

    QVERIFY(registry.declaresUse(QStringLiteral("chat_ui"), QStringLiteral("packages.show")));
    QVERIFY(!registry.declaresUse(QStringLiteral("chat_ui"), QStringLiteral("wallet.send")));
    QVERIFY(!registry.declaresUse(QStringLiteral("nobody"), QStringLiteral("packages.show")));

    QVERIFY(registry.declaresProvide(QStringLiteral("chat_ui"), QStringLiteral("chat.open")));
    QVERIFY(!registry.declaresProvide(QStringLiteral("chat_ui"), QStringLiteral("packages.show")));
}

void TestIntentRegistry::testReservedNamespaceRefusedFromApps()
{
    // Without this, any installed app could declare a shell capability and
    // intercept requests intended for the shell.
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("evil"),
        R"({"provides":[{"intent":"basecamp.repositories.manage"},{"intent":"evil.ok"}]})");

    IntentRegistry registry;
    registry.registerShellProvider(QStringLiteral("main_ui"), {}, {},
                                   QStringLiteral("Basecamp"), {});
    registry.rebuild({ { QStringLiteral("evil_ui"), plugin(dir) } }, nullptr, nullptr);

    QCOMPARE(registry.resolve(QStringLiteral("basecamp.repositories.manage")).status,
             IntentRegistry::None);
    QVERIFY(!registry.declaresProvide(QStringLiteral("evil_ui"),
                                      QStringLiteral("basecamp.repositories.manage")));

    // Its legitimate declaration is unaffected — one bad entry is not fatal.
    QVERIFY(registry.declaresProvide(QStringLiteral("evil_ui"), QStringLiteral("evil.ok")));

    bool diagnosed = false;
    for (const QString& d : registry.diagnostics())
        if (d.contains(QStringLiteral("reserved"))) diagnosed = true;
    QVERIFY(diagnosed);
}

void TestIntentRegistry::testPlatformNamespaceRefusedFromApps()
{
    // "logos." is the PLATFORM's namespace — liblogos, logoscore — and stays
    // reserved after the shell's capabilities moved to "basecamp.". Nothing
    // claims it today, which is exactly why it needs a test: an unclaimed
    // reservation is the kind that quietly stops being enforced.
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("squatter"),
        R"({"provides":[{"intent":"logos.core.restart"},{"intent":"fine.ok"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("squatter_ui"), plugin(dir) } }, nullptr, nullptr);

    QCOMPARE(registry.resolve(QStringLiteral("logos.core.restart")).status,
             IntentRegistry::None);
    QVERIFY(registry.declaresProvide(QStringLiteral("squatter_ui"),
                                     QStringLiteral("fine.ok")));
}

void TestIntentRegistry::testShellMayProvideReservedNamespace()
{
    IntentRegistry registry;
    registry.registerShellProvider(QStringLiteral("main_ui"),
                                   { QStringLiteral("basecamp.repositories.manage") }, {},
                                   QStringLiteral("Logos Basecamp"),
                                   QStringLiteral("qrc:/logo.png"));

    const auto resolution = registry.resolve(QStringLiteral("basecamp.repositories.manage"));
    QCOMPARE(resolution.status, IntentRegistry::Ok);
    QCOMPARE(resolution.found.first().moduleName, QStringLiteral("main_ui"));
    QCOMPARE(resolution.found.first().displayName, QStringLiteral("Logos Basecamp"));
}

void TestIntentRegistry::testDiskRecordCannotClaimShellIdentity()
{
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("imposter"),
        R"({"provides":[{"intent":"a.b"}]})");

    IntentRegistry registry;
    registry.registerShellProvider(QStringLiteral("main_ui"),
                                   { QStringLiteral("logos.x.y") }, {}, {}, {});
    registry.rebuild({ { QStringLiteral("main_ui"), plugin(dir) } }, nullptr, nullptr);

    // The shell's own registration is intact and the imposter contributed nothing.
    QCOMPARE(registry.resolve(QStringLiteral("logos.x.y")).status, IntentRegistry::Ok);
    QCOMPARE(registry.resolve(QStringLiteral("a.b")).status, IntentRegistry::None);
}

void TestIntentRegistry::testMalformedJsonIsDiagnosedNotFatal()
{
    QTemporaryDir root;
    const QString bad = makeApp(root, QStringLiteral("bad"), "{ this is not json ");
    const QString good = makeApp(root, QStringLiteral("good"),
        R"({"provides":[{"intent":"a.b"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("bad_ui"), plugin(bad) },
                       { QStringLiteral("good_ui"), plugin(good) } },
                     nullptr, nullptr);

    // One broken app must not take the others down with it.
    QCOMPARE(registry.resolve(QStringLiteral("a.b")).status, IntentRegistry::Ok);
    QCOMPARE(registry.diagnostics().size(), 1);
    QVERIFY(registry.diagnostics().first().contains(QStringLiteral("bad_ui")));
}

void TestIntentRegistry::testBareStringArrayIsRefused()
{
    // Tolerating two shapes is how a frozen surface stops being frozen.
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("old"),
        R"({"provides":["packages.show"]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("old_ui"), plugin(dir) } }, nullptr, nullptr);

    QCOMPARE(registry.resolve(QStringLiteral("packages.show")).status, IntentRegistry::None);
    QVERIFY(!registry.diagnostics().isEmpty());
}

void TestIntentRegistry::testInvalidNameIsRejected()
{
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("bad"),
        R"({"provides":[{"intent":"Packages.Show"},{"intent":"ok.name"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("app_ui"), plugin(dir) } }, nullptr, nullptr);

    QCOMPARE(registry.resolve(QStringLiteral("Packages.Show")).status, IntentRegistry::None);
    QCOMPARE(registry.resolve(QStringLiteral("ok.name")).status, IntentRegistry::Ok);
}

void TestIntentRegistry::testNonUiQmlIsSkipped()
{
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("legacy"),
        R"({"provides":[{"intent":"a.b"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("legacy_ui"), plugin(dir, QStringLiteral("ui")) } },
                     nullptr, nullptr);
    QCOMPARE(registry.resolve(QStringLiteral("a.b")).status, IntentRegistry::None);
}

void TestIntentRegistry::testUsesCardinalityParsedAndDiagnosed()
{
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("chat"),
        R"({"uses":[{"intent":"a.b","cardinality":"all"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("chat_ui"), plugin(dir) } }, nullptr, nullptr);

    // "all" is reserved: accepted into the file, refused in behaviour, and
    // reported so an author is not left wondering.
    QVERIFY(registry.declaresUse(QStringLiteral("chat_ui"), QStringLiteral("a.b")));
    bool diagnosed = false;
    for (const QString& d : registry.diagnostics())
        if (d.contains(QStringLiteral("cardinality"))) diagnosed = true;
    QVERIFY(diagnosed);
}

void TestIntentRegistry::testShellRegistrationSurvivesRebuild()
{
    // Installing or removing an app must not deregister the shell.
    IntentRegistry registry;
    registry.registerShellProvider(QStringLiteral("main_ui"),
                                   { QStringLiteral("logos.a.b") }, {}, {}, {});
    QCOMPARE(registry.resolve(QStringLiteral("logos.a.b")).status, IntentRegistry::Ok);

    registry.rebuild({}, nullptr, nullptr);
    QCOMPARE(registry.resolve(QStringLiteral("logos.a.b")).status, IntentRegistry::Ok);
}

void TestIntentRegistry::testProvidesCarriesTheParamShape()
{
    // A provider ships its own description of the payload. Advisory only —
    // nothing validates against it — but it is the answer a developer writing
    // a caller gets to "what does this intent need?" before published schemas.
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("wallet_ui"), R"({
        "name": "wallet_ui", "type": "ui_qml",
        "provides": [{
            "intent": "wallet.sign",
            "params": [
                {"name": "to", "type": "string", "required": true,
                 "description": "Recipient address"},
                {"type": "string"}
            ]
        }]
    })");
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("wallet_ui"), plugin(dir) } },
                     nullptr, nullptr);

    // The capability still resolves — the shape does not affect dispatch.
    QCOMPARE(registry.resolve(QStringLiteral("wallet.sign")).status,
             IntentRegistry::Ok);

    const QVariantList specs = registry.paramsSpecFor(
        QStringLiteral("wallet_ui"), QStringLiteral("wallet.sign"));
    QCOMPARE(specs.size(), 1);   // the nameless entry tells a caller nothing
    QCOMPARE(specs.first().toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("to"));
    QVERIFY(specs.first().toMap().value(QStringLiteral("required")).toBool());

    // A provider that described nothing is not claiming "takes nothing".
    QVERIFY(registry.paramsSpecFor(QStringLiteral("wallet_ui"),
                                   QStringLiteral("nope.nope")).isEmpty());
}

void TestIntentRegistry::testRestrictedIntentAllowsOnlyListedRequesters()
{
    IntentRegistry registry;

    // Unrestricted by default — a restriction is opt-in, never implied.
    QVERIFY(registry.requesterAllowed(QStringLiteral("basecamp.packages.confirm_uninstall"),
                                      QStringLiteral("evil_ui")));

    registry.restrictIntentToRequesters(
        QStringLiteral("basecamp.packages.confirm_uninstall"),
        { QStringLiteral("package_manager_ui") });

    QVERIFY(registry.requesterAllowed(QStringLiteral("basecamp.packages.confirm_uninstall"),
                                      QStringLiteral("package_manager_ui")));
    QVERIFY(!registry.requesterAllowed(QStringLiteral("basecamp.packages.confirm_uninstall"),
                                       QStringLiteral("evil_ui")));

    // Byte-exact, like every other name comparison on this surface.
    QVERIFY(!registry.requesterAllowed(QStringLiteral("basecamp.packages.confirm_uninstall"),
                                       QStringLiteral("Package_Manager_UI")));

    // Restricting one intent must not touch its siblings.
    QVERIFY(registry.requesterAllowed(QStringLiteral("basecamp.packages.confirm_install"),
                                      QStringLiteral("evil_ui")));
}

void TestIntentRegistry::testEmptyRequesterListIsRefusedNotAnOpenDoor()
{
    // An empty list reads as "restricted to nobody" but would store as
    // "unrestricted". Refusing it is what stops a typo from silently opening a
    // destructive capability to every installed app.
    IntentRegistry registry;
    registry.restrictIntentToRequesters(
        QStringLiteral("basecamp.packages.confirm_uninstall"), {});

    QVERIFY(registry.requesterAllowed(QStringLiteral("basecamp.packages.confirm_uninstall"),
                                      QStringLiteral("evil_ui")));
    QVERIFY(!registry.diagnostics().isEmpty());
}

void TestIntentRegistry::testRestrictionSurvivesRebuild()
{
    // The restriction is code-declared policy, not a disk record, so a rebuild
    // triggered by any install/uninstall must not drop it.
    IntentRegistry registry;
    registry.restrictIntentToRequesters(
        QStringLiteral("basecamp.packages.confirm_uninstall"),
        { QStringLiteral("package_manager_ui") });

    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("evil_ui"), R"({
        "name": "evil_ui", "type": "ui_qml",
        "uses": [{"intent": "basecamp.packages.confirm_uninstall"}]
    })");
    registry.rebuild({ { QStringLiteral("evil_ui"), plugin(dir) } }, nullptr, nullptr);

    QVERIFY(!registry.requesterAllowed(QStringLiteral("basecamp.packages.confirm_uninstall"),
                                       QStringLiteral("evil_ui")));
}

void TestIntentRegistry::testHandoffIsReadPerProviderIntent()
{
    // Does servicing this intent END, or does it hand the user off? Absent is
    // false — the common case is a transaction, and opting out is the rare one.
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("wallet_ui"), R"({
        "name": "wallet_ui", "type": "ui_qml",
        "provides": [
            {"intent": "wallet.sign"},
            {"intent": "wallet.open", "handoff": true},
            {"intent": "wallet.close", "handoff": false}
        ]
    })");
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("wallet_ui"), plugin(dir) } }, nullptr, nullptr);

    QVERIFY(registry.isHandoff(QStringLiteral("wallet_ui"), QStringLiteral("wallet.open")));
    QVERIFY(!registry.isHandoff(QStringLiteral("wallet_ui"), QStringLiteral("wallet.sign")));
    QVERIFY(!registry.isHandoff(QStringLiteral("wallet_ui"), QStringLiteral("wallet.close")));
    // An intent this provider does not declare at all.
    QVERIFY(!registry.isHandoff(QStringLiteral("wallet_ui"), QStringLiteral("nope.nope")));
}

void TestIntentRegistry::testNonBooleanHandoffIsDiagnosedNotCoerced()
{
    // QVariant("true").toBool() is true, so coercing would silently give a
    // transactional intent the opposite navigation from the one its author
    // wrote. Refuse and say so.
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("wallet_ui"), R"({
        "name": "wallet_ui", "type": "ui_qml",
        "provides": [{"intent": "wallet.open", "handoff": "true"}]
    })");
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("wallet_ui"), plugin(dir) } }, nullptr, nullptr);

    QVERIFY(!registry.isHandoff(QStringLiteral("wallet_ui"), QStringLiteral("wallet.open")));
    // The capability itself still registers — a bad flag is not a bad intent.
    QCOMPARE(registry.resolve(QStringLiteral("wallet.open")).status, IntentRegistry::Ok);
    QVERIFY(!registry.diagnostics().filter(QStringLiteral("handoff")).isEmpty());
}

void TestIntentRegistry::testProvidersMayDisagreeAboutHandoff()
{
    // Per (provider, intent), like paramsSpecFor and for the same reason: two
    // apps may implement one capability differently and there is no per-intent
    // schema to arbitrate. The chosen provider's answer is the one that counts.
    QTemporaryDir root;
    const QString a = makeApp(root, QStringLiteral("a_ui"), R"({
        "name": "a_ui", "type": "ui_qml",
        "provides": [{"intent": "notes.open", "handoff": true}]
    })");
    const QString b = makeApp(root, QStringLiteral("b_ui"), R"({
        "name": "b_ui", "type": "ui_qml",
        "provides": [{"intent": "notes.open"}]
    })");
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("a_ui"), plugin(a) },
                       { QStringLiteral("b_ui"), plugin(b) } }, nullptr, nullptr);

    QVERIFY(registry.isHandoff(QStringLiteral("a_ui"), QStringLiteral("notes.open")));
    QVERIFY(!registry.isHandoff(QStringLiteral("b_ui"), QStringLiteral("notes.open")));
    QCOMPARE(registry.resolve(QStringLiteral("notes.open")).status,
             IntentRegistry::Ambiguous);
}

void TestIntentRegistry::testShellHandoffSurvivesRebuild()
{
    // The shell's declaration is code, not disk, so installing an app must not
    // quietly turn its hand-off back into a transaction.
    IntentRegistry registry;
    registry.registerShellProvider(QStringLiteral("main_ui"),
                                   { QStringLiteral("basecamp.repositories.manage"),
                                     QStringLiteral("basecamp.packages.confirm_install") },
                                   { QStringLiteral("basecamp.repositories.manage") },
                                   QStringLiteral("Logos"), {});

    QVERIFY(registry.isHandoff(QStringLiteral("main_ui"),
                               QStringLiteral("basecamp.repositories.manage")));
    QVERIFY(!registry.isHandoff(QStringLiteral("main_ui"),
                                QStringLiteral("basecamp.packages.confirm_install")));

    registry.rebuild({}, nullptr, nullptr);

    QVERIFY(registry.isHandoff(QStringLiteral("main_ui"),
                               QStringLiteral("basecamp.repositories.manage")));
    QVERIFY(!registry.isHandoff(QStringLiteral("main_ui"),
                                QStringLiteral("basecamp.packages.confirm_install")));
}

// ── Catalog-sourced providers ───────────────────────────────────────────────
//
// The table the RESOLVER never consults: these packages are not installed, so
// nothing can be dispatched to them. It feeds the install suggestion and the
// chooser's "Not installed" section.

// The chooser renders installed and installable providers as two sections,
// which only reads correctly if a package cannot appear in both. The registry
// is what guarantees that — and without it the shell would offer to install
// something already on disk.
void TestIntentRegistry::testInstallableNeverOverlapsAnInstalledPackage()
{
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("wallet"),
        R"({"name":"wallet_ui","provides":[{"intent":"wallet.send"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("wallet_ui"), plugin(dir) } },
                     [](const QString& n) { return n; },
                     [](const QString&) { return QString(); });

    // The catalog offers the installed one AND a second that is not.
    registry.setInstallableProviders({
        { QStringLiteral("wallet_ui"), { QStringLiteral("wallet.send") } },
        { QStringLiteral("secure_ui"), { QStringLiteral("wallet.send") } },
    });

    QCOMPARE(registry.installableProvidersFor(QStringLiteral("wallet.send")),
             QStringList{ QStringLiteral("secure_ui") });

    // …and the installed one is still the only thing that resolves.
    const auto resolution = registry.resolve(QStringLiteral("wallet.send"));
    QCOMPARE(resolution.status, IntentRegistry::Ok);
    QCOMPARE(resolution.found.size(), 1);
    QCOMPARE(resolution.found.first().moduleName, QStringLiteral("wallet_ui"));
}

// A catalog is a LESS trusted source than the local disk, so it gets the same
// filter and not a laxer one. Reaching the chooser is enough to matter: an
// entry claiming `basecamp.*` would put a shell capability into a list of
// things the user is invited to install.
void TestIntentRegistry::testInstallableRefusesReservedAndInvalidNames()
{
    IntentRegistry registry;
    registry.setInstallableProviders({
        { QStringLiteral("evil_ui"), { QStringLiteral("basecamp.settings.open"),
                                       QStringLiteral("logos.anything"),
                                       QStringLiteral("not a valid name"),
                                       QStringLiteral("wallet.send") } },
    });

    QVERIFY(registry.installableProvidersFor(QStringLiteral("basecamp.settings.open")).isEmpty());
    QVERIFY(registry.installableProvidersFor(QStringLiteral("logos.anything")).isEmpty());
    QVERIFY(registry.installableProvidersFor(QStringLiteral("not a valid name")).isEmpty());

    // The well-formed entry in the same record still lands: one bad name does
    // not discard the package.
    QCOMPARE(registry.installableProvidersFor(QStringLiteral("wallet.send")),
             QStringList{ QStringLiteral("evil_ui") });
}

// Order is what the user reads, so it comes from the data rather than from
// whichever order the catalog happened to arrive in.
void TestIntentRegistry::testInstallableIsSortedAndDeduplicated()
{
    IntentRegistry registry;
    registry.setInstallableProviders({
        { QStringLiteral("zeta_ui"),  { QStringLiteral("wallet.send") } },
        { QStringLiteral("alpha_ui"), { QStringLiteral("wallet.send"),
                                        QStringLiteral("wallet.send") } },
    });

    QCOMPARE(registry.installableProvidersFor(QStringLiteral("wallet.send")),
             (QStringList{ QStringLiteral("alpha_ui"), QStringLiteral("zeta_ui") }));
}

QTEST_MAIN(TestIntentRegistry)
#include "intent_registry_test.moc"
