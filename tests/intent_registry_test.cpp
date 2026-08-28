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
        R"({"provides":[{"intent":"logos.repositories.manage"},{"intent":"evil.ok"}]})");

    IntentRegistry registry;
    registry.registerShellProvider(QStringLiteral("main_ui"), {}, QStringLiteral("Basecamp"), {});
    registry.rebuild({ { QStringLiteral("evil_ui"), plugin(dir) } }, nullptr, nullptr);

    QCOMPARE(registry.resolve(QStringLiteral("logos.repositories.manage")).status,
             IntentRegistry::None);
    QVERIFY(!registry.declaresProvide(QStringLiteral("evil_ui"),
                                      QStringLiteral("logos.repositories.manage")));

    // Its legitimate declaration is unaffected — one bad entry is not fatal.
    QVERIFY(registry.declaresProvide(QStringLiteral("evil_ui"), QStringLiteral("evil.ok")));

    bool diagnosed = false;
    for (const QString& d : registry.diagnostics())
        if (d.contains(QStringLiteral("reserved"))) diagnosed = true;
    QVERIFY(diagnosed);
}

void TestIntentRegistry::testShellMayProvideReservedNamespace()
{
    IntentRegistry registry;
    registry.registerShellProvider(QStringLiteral("main_ui"),
                                   { QStringLiteral("logos.repositories.manage") },
                                   QStringLiteral("Logos Basecamp"),
                                   QStringLiteral("qrc:/logo.png"));

    const auto resolution = registry.resolve(QStringLiteral("logos.repositories.manage"));
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
                                   { QStringLiteral("logos.x.y") }, {}, {});
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
                                   { QStringLiteral("logos.a.b") }, {}, {});
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
    QVERIFY(registry.requesterAllowed(QStringLiteral("logos.packages.confirm_uninstall"),
                                      QStringLiteral("evil_ui")));

    registry.restrictIntentToRequesters(
        QStringLiteral("logos.packages.confirm_uninstall"),
        { QStringLiteral("package_manager_ui") });

    QVERIFY(registry.requesterAllowed(QStringLiteral("logos.packages.confirm_uninstall"),
                                      QStringLiteral("package_manager_ui")));
    QVERIFY(!registry.requesterAllowed(QStringLiteral("logos.packages.confirm_uninstall"),
                                       QStringLiteral("evil_ui")));

    // Byte-exact, like every other name comparison on this surface.
    QVERIFY(!registry.requesterAllowed(QStringLiteral("logos.packages.confirm_uninstall"),
                                       QStringLiteral("Package_Manager_UI")));

    // Restricting one intent must not touch its siblings.
    QVERIFY(registry.requesterAllowed(QStringLiteral("logos.packages.confirm_install"),
                                      QStringLiteral("evil_ui")));
}

void TestIntentRegistry::testEmptyRequesterListIsRefusedNotAnOpenDoor()
{
    // An empty list reads as "restricted to nobody" but would store as
    // "unrestricted". Refusing it is what stops a typo from silently opening a
    // destructive capability to every installed app.
    IntentRegistry registry;
    registry.restrictIntentToRequesters(
        QStringLiteral("logos.packages.confirm_uninstall"), {});

    QVERIFY(registry.requesterAllowed(QStringLiteral("logos.packages.confirm_uninstall"),
                                      QStringLiteral("evil_ui")));
    QVERIFY(!registry.diagnostics().isEmpty());
}

void TestIntentRegistry::testRestrictionSurvivesRebuild()
{
    // The restriction is code-declared policy, not a disk record, so a rebuild
    // triggered by any install/uninstall must not drop it.
    IntentRegistry registry;
    registry.restrictIntentToRequesters(
        QStringLiteral("logos.packages.confirm_uninstall"),
        { QStringLiteral("package_manager_ui") });

    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("evil_ui"), R"({
        "name": "evil_ui", "type": "ui_qml",
        "uses": [{"intent": "logos.packages.confirm_uninstall"}]
    })");
    registry.rebuild({ { QStringLiteral("evil_ui"), plugin(dir) } }, nullptr, nullptr);

    QVERIFY(!registry.requesterAllowed(QStringLiteral("logos.packages.confirm_uninstall"),
                                       QStringLiteral("evil_ui")));
}

QTEST_MAIN(TestIntentRegistry)
#include "intent_registry_test.moc"
