// srcdeps: links/LinkRequestCoordinator.cpp links/LinkUrl.cpp links/LinkUrlInbox.cpp IntentBroker.cpp IntentRegistry.cpp ShellIntents.cpp
//
// A clicked `basecamp://` URL, from the inbox through to the broker, against
// the real registry and broker with the same fakes intent_broker_test uses.
//
// THE CASE THIS FILE EXISTS FOR is testLinkFacesTheChooser. The broker skips
// the chooser when the shell is either party to a request; the link requester
// is registered under its own name precisely so it does NOT inherit that, and a
// regression there would make every web link dispatch into an installed app
// with no consent step at all. It would not fail any other test, and it would
// not look wrong in a diff.
//
// The other load-bearing one is testWebReachabilityIsOptIn: reachability is
// declared by the PROVIDER, per intent, because `uses` cannot gate a link —
// there is no manifest on the calling side. Without it every entry in every
// installed app's `provides` silently becomes a web entry point.

#include "IntentBroker.h"
#include "IntentRegistry.h"
#include "ShellIntents.h"
#include "ShellIntentEndpoint.h"
#include "links/LinkRequestCoordinator.h"
#include "links/LinkUrlInbox.h"

#include <memory>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

class FakeChooser : public IntentChooser {
public:
    QStringList presented;
    QVariantList lastProviders;
    int present(const QString& dispatchId, const QString&, const QString&,
                const QVariantList& providers) override
    {
        presented.append(dispatchId);
        lastProviders = providers;
        return 1;
    }
    void dismiss(const QString&) override {}
};

class FakePresenter : public IntentPresenter {
public:
    QStringList presentedApps;
    bool isAppLoaded(const QString& n) const override { return loaded.contains(n); }
    void ensureAppLoaded(const QString& n) override { loaded.append(n); }
    void presentApp(const QString& n) override { presentedApps.append(n); }
    bool isAppFrontmost(const QString&) const override { return false; }
    bool anyDialogOpen() const override { return false; }
    QStringList loaded;
};

class FakeEndpoint : public IntentEndpoint {
public:
    QStringList delivered;
    int deliverRequest(const QString&, const QString& intent, const QVariantMap&,
                       const QString&) override
    {
        delivered.append(intent);
        return 1;
    }
    void deliverResult(const QString&, const QVariantMap&) override {}
    QObject* asObject() override { return nullptr; }
};

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

QVariantMap plugin(const QString& dir)
{
    return QVariantMap{ { QStringLiteral("installDir"), dir },
                        { QStringLiteral("type"), QStringLiteral("ui_qml") } };
}

void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

} // namespace

class TestLinkIntent : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void testLinkFacesTheChooser();
    void testOneAppCannotPublishAnothersCapability();
    void testWebReachabilityIsOptIn();
    void testNonBooleanWebIsDiagnosedNotCoerced();
    void testShellConfirmIntentsAreNotWebReachable();
    void testRestrictedIntentsDenyTheLinkRequester();
    void testDiskRecordCannotClaimTheLinkIdentity();
    void testUrlIsParkedUntilTheRegistryIsReady();
    void testParkedUrlFailsAfterTheDeadline();
    void testOnlyOneLinkRequestInFlight();
    void testBareUrlRaisesWithoutSubmitting();
    void testSurvivesTheBrokerBeingDestroyedFirst();

private:
    // Registry with the shell's real registration plus the link requester,
    // exactly as MainUIBackend wires them.
    IntentRegistry* wired(QObject* parent)
    {
        auto* registry = new IntentRegistry(parent);
        ShellIntents::registerWith(registry, QStringLiteral("main_ui"),
                                   QStringLiteral("Logos"), QStringLiteral("qrc:/x.svg"));
        registry->registerLinkRequester(LinkRequestCoordinator::requesterName(),
                                        ShellIntents::kWebReachableIntents);
        return registry;
    }
};

void TestLinkIntent::init()
{
    // The inbox is process-wide, so a URL left over from a previous case would
    // leak into the next one.
    LinkUrlInbox::instance().takeAll();
}

void TestLinkIntent::testLinkFacesTheChooser()
{
    QObject owner;
    QTemporaryDir root;

    // One installed app, opted in. One provider only — which is exactly the
    // case the shell is exempted from, so if the link requester were ever
    // treated as the shell this would dispatch straight through instead.
    const QString dir = makeApp(root, QStringLiteral("wallet"),
        R"({"provides":[{"intent":"wallet.send","web":true}]})");

    IntentRegistry* registry = wired(&owner);
    registry->rebuild({ { QStringLiteral("wallet_ui"), plugin(dir) } }, nullptr, nullptr);

    FakePresenter presenter;
    FakeChooser chooser;
    IntentBroker broker(registry, &presenter, &owner);
    broker.setChooser(&chooser);

    FakeEndpoint provider;
    broker.registerEndpoint(QStringLiteral("wallet_ui"), &provider);

    ShellIntentEndpoint link(ShellIntentEndpoint::DeliverFn{},
                            [](const QString&, const QVariantMap&) {});
    broker.registerEndpoint(LinkRequestCoordinator::requesterName(), &link);

    broker.submit(&link, QStringLiteral("link-1"), QStringLiteral("wallet.send"), {});
    spin(200);

    QCOMPARE(chooser.presented.size(), 1);
    QVERIFY2(provider.delivered.isEmpty(),
             "a link dispatched without the user being asked");
}

void TestLinkIntent::testOneAppCannotPublishAnothersCapability()
{
    QObject owner;
    QTemporaryDir root;
    const QString opted = makeApp(root, QStringLiteral("opted"),
        R"({"provides":[{"intent":"wallet.send","web":true}]})");
    const QString never = makeApp(root, QStringLiteral("never"),
        R"({"provides":[{"intent":"wallet.send"}]})");

    IntentRegistry* registry = wired(&owner);
    registry->rebuild({ { QStringLiteral("opted_ui"), plugin(opted) },
                        { QStringLiteral("never_ui"), plugin(never) } },
                      nullptr, nullptr);

    // THE ESCALATION THIS PINS. `web` used to be keyed on the intent name
    // alone, so one app setting the flag published EVERY provider of that name
    // — including apps whose authors deliberately left it off. The opt-in is a
    // statement about your own app, so it is recorded against your own app.
    const auto forLink = registry->resolveFor(
        LinkRequestCoordinator::requesterName(), QStringLiteral("wallet.send"));
    QCOMPARE(forLink.found.size(), 1);
    QCOMPARE(forLink.found.first().moduleName, QStringLiteral("opted_ui"));
    QCOMPARE(forLink.status, IntentRegistry::Ok);

    // An ordinary app requester is unaffected — both still resolve, and the
    // user still chooses. `web` narrows link origin only.
    const auto forApp = registry->resolveFor(QStringLiteral("chat_ui"),
                                             QStringLiteral("wallet.send"));
    QCOMPARE(forApp.found.size(), 2);
    QCOMPARE(forApp.status, IntentRegistry::Ambiguous);

    QVERIFY(registry->isWebReachable(QStringLiteral("opted_ui"),
                                     QStringLiteral("wallet.send")));
    QVERIFY(!registry->isWebReachable(QStringLiteral("never_ui"),
                                      QStringLiteral("wallet.send")));
}

void TestLinkIntent::testWebReachabilityIsOptIn()
{
    QObject owner;
    QTemporaryDir root;

    // Same app, same capability — the only difference is the flag.
    const QString opted = makeApp(root, QStringLiteral("yes"),
        R"({"provides":[{"intent":"wallet.send","web":true}]})");
    const QString silent = makeApp(root, QStringLiteral("no"),
        R"({"provides":[{"intent":"notes.write"}]})");

    IntentRegistry* registry = wired(&owner);
    registry->rebuild({ { QStringLiteral("yes_ui"),  plugin(opted) },
                        { QStringLiteral("no_ui"),   plugin(silent) } },
                      nullptr, nullptr);

    const QString link = LinkRequestCoordinator::requesterName();
    QVERIFY(registry->declaresUse(link, QStringLiteral("wallet.send")));
    QVERIFY2(!registry->declaresUse(link, QStringLiteral("notes.write")),
             "an intent that never opted in became reachable from a URL");

    QVERIFY(registry->isWebReachable(QStringLiteral("yes_ui"), QStringLiteral("wallet.send")));
    QVERIFY(!registry->isWebReachable(QStringLiteral("no_ui"), QStringLiteral("notes.write")));
}

void TestLinkIntent::testNonBooleanWebIsDiagnosedNotCoerced()
{
    QObject owner;
    QTemporaryDir root;

    // `"web": "false"` read through QVariant::toBool() is TRUE, which would
    // publish a capability whose author was explicitly declining to publish it.
    const QString dir = makeApp(root, QStringLiteral("sloppy"),
        R"({"provides":[{"intent":"notes.write","web":"false"}]})");

    IntentRegistry* registry = wired(&owner);
    registry->rebuild({ { QStringLiteral("sloppy_ui"), plugin(dir) } }, nullptr, nullptr);

    QVERIFY(!registry->isWebReachable(QStringLiteral("sloppy_ui"), QStringLiteral("notes.write")));

    bool diagnosed = false;
    for (const QString& d : registry->diagnostics())
        if (d.contains(QStringLiteral("web"))) diagnosed = true;
    QVERIFY(diagnosed);
}

void TestLinkIntent::testShellConfirmIntentsAreNotWebReachable()
{
    QObject owner;
    IntentRegistry* registry = wired(&owner);
    registry->rebuild({}, nullptr, nullptr);

    const QString link = LinkRequestCoordinator::requesterName();

    // Navigation is the safe set and is reachable.
    for (const QString& nav : ShellIntents::kNavigationIntents)
        QVERIFY2(registry->declaresUse(link, nav), qPrintable(nav));

    // Installing and removing packages is not. A web page must not be able to
    // raise these at all, let alone without a chooser.
    for (const QString& confirm : ShellIntents::kPackageConfirmIntents) {
        QVERIFY2(!registry->declaresUse(link, confirm), qPrintable(confirm));
        QVERIFY2(!registry->isWebReachable(QStringLiteral("main_ui"), confirm), qPrintable(confirm));
    }
}

void TestLinkIntent::testRestrictedIntentsDenyTheLinkRequester()
{
    QObject owner;
    IntentRegistry* registry = wired(&owner);
    registry->rebuild({}, nullptr, nullptr);

    // Belt as well as braces: even if one of these were ever added to the
    // web-reachable set by mistake, the requester allow-list still refuses it
    // because the link requester is not package_manager_ui.
    const QString link = LinkRequestCoordinator::requesterName();
    for (const QString& restricted : ShellIntents::kRestrictedToPackageManagerUi)
        QVERIFY2(!registry->requesterAllowed(restricted, link), qPrintable(restricted));
}

void TestLinkIntent::testDiskRecordCannotClaimTheLinkIdentity()
{
    QObject owner;
    QTemporaryDir root;

    // That name's `uses` IS the web-reachable set, so a package answering to it
    // could add itself entries and reach its own capabilities from a URL
    // without ever declaring `"web": true`.
    const QString dir = makeApp(root, QStringLiteral("squatter"),
        R"({"uses":[{"intent":"notes.write"}],"provides":[{"intent":"notes.write"}]})");

    IntentRegistry* registry = wired(&owner);
    registry->rebuild(
        { { LinkRequestCoordinator::requesterName(), plugin(dir) } }, nullptr, nullptr);

    const QString link = LinkRequestCoordinator::requesterName();
    QVERIFY(!registry->declaresUse(link, QStringLiteral("notes.write")));
    QVERIFY(!registry->declaresProvide(link, QStringLiteral("notes.write")));
    // Its own registration survived.
    QVERIFY(registry->declaresUse(link, QStringLiteral("basecamp.settings.open")));
}

void TestLinkIntent::testUrlIsParkedUntilTheRegistryIsReady()
{
    QObject owner;
    IntentRegistry* registry = wired(&owner);
    registry->rebuild({}, nullptr, nullptr);

    FakePresenter presenter;
    FakeChooser chooser;
    IntentBroker broker(registry, &presenter, &owner);
    broker.setChooser(&chooser);

    LinkRequestCoordinator coordinator(&broker, registry, &owner);

    // Posted before the gate opens — the cold-start case, where the click that
    // launched the app lands seconds before the registry knows anything.
    LinkUrlInbox::instance().post(QStringLiteral("basecamp://intent/basecamp.settings.open"));
    spin(100);
    QCOMPARE(broker.pendingCount(), 0);
    QVERIFY2(!LinkUrlInbox::instance().isEmpty(), "the URL was consumed before the gate");

    coordinator.onRegistryReady();
    spin(200);

    // Now it went through. Shell-provided, so no chooser — but it was submitted.
    QVERIFY(LinkUrlInbox::instance().isEmpty());
}

void TestLinkIntent::testParkedUrlFailsAfterTheDeadline()
{
    QObject owner;
    IntentRegistry* registry = wired(&owner);
    registry->rebuild({}, nullptr, nullptr);

    FakePresenter presenter;
    IntentBroker broker(registry, &presenter, &owner);

    LinkRequestCoordinator coordinator(&broker, registry, &owner);
    coordinator.setParkDeadlineMs(50);
    QSignalSpy failures(&coordinator, &LinkRequestCoordinator::linkFailed);

    // A URL waiting on a rebuild that never comes must report, not evaporate.
    // Status has no equivalent: their stash is dropped in silence if login
    // never completes, and the click looks like it did nothing.
    LinkUrlInbox::instance().post(QStringLiteral("basecamp://intent/basecamp.settings.open"));
    spin(300);

    QCOMPARE(failures.count(), 1);
    QVERIFY(LinkUrlInbox::instance().isEmpty());
}

void TestLinkIntent::testOnlyOneLinkRequestInFlight()
{
    QObject owner;
    QTemporaryDir root;
    const QString dir = makeApp(root, QStringLiteral("wallet"),
        R"({"provides":[{"intent":"wallet.send","web":true}]})");

    IntentRegistry* registry = wired(&owner);
    registry->rebuild({ { QStringLiteral("wallet_ui"), plugin(dir) } }, nullptr, nullptr);

    FakePresenter presenter;
    FakeChooser chooser;
    IntentBroker broker(registry, &presenter, &owner);
    broker.setChooser(&chooser);

    FakeEndpoint provider;
    broker.registerEndpoint(QStringLiteral("wallet_ui"), &provider);

    LinkRequestCoordinator coordinator(&broker, registry, &owner);
    coordinator.onRegistryReady();
    QSignalSpy failures(&coordinator, &LinkRequestCoordinator::linkFailed);

    // The broker QUEUES choosers rather than refusing them, so without the cap
    // a page navigating repeatedly would stack consent dialogs.
    for (int i = 0; i < 4; ++i)
        LinkUrlInbox::instance().post(QStringLiteral("basecamp://intent/wallet.send"));
    spin(300);

    QCOMPARE(chooser.presented.size(), 1);
    QVERIFY2(failures.count() >= 1, "the dropped links were dropped silently");
}

void TestLinkIntent::testBareUrlRaisesWithoutSubmitting()
{
    QObject owner;
    IntentRegistry* registry = wired(&owner);
    registry->rebuild({}, nullptr, nullptr);

    FakePresenter presenter;
    IntentBroker broker(registry, &presenter, &owner);

    LinkRequestCoordinator coordinator(&broker, registry, &owner);
    coordinator.onRegistryReady();

    int raised = 0;
    coordinator.setRaiseHandler([&raised]() { ++raised; });

    LinkUrlInbox::instance().post(QStringLiteral("basecamp://"));
    spin(150);

    QCOMPARE(raised, 1);
    QCOMPARE(broker.pendingCount(), 0);
}

void TestLinkIntent::testSurvivesTheBrokerBeingDestroyedFirst()
{
    // THE SHUTDOWN CRASH, reproduced. Every quit path — SIGTERM, SIGINT, ⌘Q —
    // segfaulted in IntentBroker::endpointDestroyed with
    // KERN_INVALID_ADDRESS, because ~LinkRequestCoordinator called into a
    // broker that was already gone.
    //
    // The premise that was wrong: Qt does NOT delete children in reverse
    // construction order. QObjectPrivate::deleteChildren() walks the list from
    // index 0, so MainUIBackend frees the broker (its 2nd child) long before
    // this coordinator (its last). Ordering cannot be relied on to keep the
    // pointer valid, so it is a QPointer and the destructor checks it.
    QObject owner;
    IntentRegistry* registry = wired(&owner);
    registry->rebuild({}, nullptr, nullptr);

    FakePresenter presenter;
    auto* broker = new IntentBroker(registry, &presenter, nullptr);
    auto* coordinator = new LinkRequestCoordinator(broker, registry, nullptr);

    // Exactly the order MainUIBackend produces.
    delete broker;
    delete coordinator;   // must not touch the freed broker

    QVERIFY(true);        // reaching here at all is the assertion
}

QTEST_MAIN(TestLinkIntent)
#include "link_intent_test.moc"
