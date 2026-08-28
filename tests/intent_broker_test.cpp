// srcdeps: IntentBroker.cpp IntentRegistry.cpp
//
// The broker drives fakes and nothing else — no bridge, no LogosAPI, no
// widgets. Deadlines are shortened so the timeout cases are affordable.
//
// The case this file exists for is testSpoofedResponseIsIgnored: a provider
// that guesses or replays a dispatch id must not be able to answer another
// app's request. Everything else is ordinary coverage.

#include "IntentBroker.h"
#include "IntentRegistry.h"

#include <memory>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

class FakeEndpoint : public IntentEndpoint {
public:
    struct Delivered {
        QString dispatchId;
        QString intent;
        QVariantMap params;
        QString requesterName;
    };

    QList<Delivered> requests;
    QList<QPair<QString, QVariantMap>> results;
    int receiverCount = 1;   // pretend the view has a handler

    int deliverRequest(const QString& dispatchId, const QString& intent,
                       const QVariantMap& params, const QString& requesterName) override
    {
        requests.append({dispatchId, intent, params, requesterName});
        return receiverCount;
    }

    void deliverResult(const QString& requestId, const QVariantMap& envelope) override
    {
        results.append({requestId, envelope});
    }

    QObject* asObject() override { return nullptr; }

    bool ok() const { return results.value(0).second.value(QStringLiteral("ok")).toBool(); }
    QString error() const
    {
        return results.value(0).second.value(QStringLiteral("error")).toString();
    }
};

class FakeChooser : public IntentChooser {
public:
    // `mounted` models what the real seam reports: whether a dialog actually
    // exists, NOT whether a signal happens to be connected.
    bool mounted = true;
    QStringList presented;
    QStringList dismissed;
    QVariantList lastProviders;

    int present(const QString& dispatchId, const QString&, const QString&,
                const QVariantList& providers) override
    {
        if (!mounted) return 0;
        presented.append(dispatchId);
        lastProviders = providers;
        return 1;
    }
    void dismiss(const QString& dispatchId) override { dismissed.append(dispatchId); }
};

class FakeInstaller : public IntentInstaller {
public:
    // No `mounted` flag, unlike FakeChooser: whether a prompt is on screen no
    // longer changes anything, because the request is already answered.
    QStringList offered;          // intents suggested, in order
    QStringList lastCandidates;

    void offerInstall(const QString& intent,
                      const QStringList& candidates) override
    {
        offered.append(intent);
        lastCandidates = candidates;
    }
};

class FakePresenter : public IntentPresenter {
public:
    QStringList loaded;
    QStringList ensureCalls;
    QStringList presented;

    bool isAppLoaded(const QString& appName) const override { return loaded.contains(appName); }
    void ensureAppLoaded(const QString& appName) override { ensureCalls.append(appName); }
    void presentApp(const QString& appName) override { presented.append(appName); }
};

QString writeApp(QTemporaryDir& root, const QString& dirName, const QByteArray& json)
{
    const QString dir = root.filePath(dirName);
    QDir().mkpath(dir);
    QFile file(QDir(dir).filePath(QStringLiteral("metadata.json")));
    file.open(QIODevice::WriteOnly);
    file.write(json);
    file.close();
    return dir;
}

QVariantMap plugin(const QString& installDir)
{
    return QVariantMap{ { QStringLiteral("installDir"), installDir },
                        { QStringLiteral("type"), QStringLiteral("ui_qml") } };
}

void spin(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

// The shell confirms every dispatch, the single-provider case included, so a
// test about what happens AFTER dispatch has to answer the chooser to get there.
void submitAndConfirm(IntentBroker& broker, FakeChooser& chooser,
                      IntentEndpoint* from, const QString& requestId,
                      const QString& intent, const QString& provider)
{
    broker.submit(from, requestId, intent, {});
    spin(80);
    broker.resolveChooser(chooser.presented.last(), provider);
    spin(80);
}

} // namespace

class TestIntentBroker : public QObject {
    Q_OBJECT

private:
    // chat_ui uses wallet.send; wallet_a and wallet_b both provide it.
    void buildTwoProviderWorld(QTemporaryDir& root, IntentRegistry& registry);
    // Same, minus wallet_b. Resolution is then unambiguous — the confirmation
    // still comes up, but with one candidate there is nothing to pick wrong.
    void buildOneProviderWorld(QTemporaryDir& root, IntentRegistry& registry);

private slots:
    void testNotDeclaredShortCircuits();
    void testNoProviderIsUnavailableAfterFloor();
    void testRestrictedRequesterIsDeniedIndistinguishably();
    void testHappyPathDispatchesAndRoutesBack();
    void testDispatchIdDiffersFromRequestId();
    void testSpoofedResponseIsIgnored();
    void testResponseFromWrongEndpointIsIgnored();
    void testSecondResponseIsIgnored();
    void testAmbiguousRaisesChooserAndPresentsNothing();
    void testChooserResolutionDispatches();
    void testChooserCancelIsCancelled();
    void testActivationQueueLoadsOnce();
    void testProviderFreeTextBecomesFailed();
    void testAmbiguousNeverReachesTheEnvelope();
    void testProviderDeathIsUnavailableNotFailed();
    void testRequesterDeathDropsSilently();
    void testAmbiguousWithNoChooserFailsRatherThanHangs();
    void testShellProviderIsNeverLoadedOrPresented();
    void testAnAppProvidingItsOwnIntentSkipsTheChooser();
    void testNonCanonicalParamsAreRefusedAsBadRequest();
    void testBadRequestIsFlooredLikeUnavailable();
    void testDeclaredParamsAreEnforcedAtDispatch();
    void testUndescribedAndExtraParamsStillDispatch();
    void testProviderThatNeverAnswersTimesOut();
    void testProviderWithNoHandlerAlsoTimesOut();
    void testActivationThatNeverCompletesEndsUnavailable();
    void testInstallableProviderIsOfferedNotDispatched();
    void testAnOfferIsIndistinguishableFromNoProviderAtAll();
    void testInstallableProvidersNeverReachResolve();
    void testSecondChoiceQueuesInsteadOfRepointingTheDialog();
    void testAmbiguousWithChooserAsksAndRoutesTheChoice();
};

void TestIntentBroker::buildTwoProviderWorld(QTemporaryDir& root, IntentRegistry& registry)
{
    const QString chat = writeApp(root, QStringLiteral("chat"),
        R"({"uses":[{"intent":"wallet.send"}]})");
    const QString wa = writeApp(root, QStringLiteral("wa"),
        R"({"provides":[{"intent":"wallet.send"}]})");
    const QString wb = writeApp(root, QStringLiteral("wb"),
        R"({"provides":[{"intent":"wallet.send"}]})");

    registry.rebuild({ { QStringLiteral("chat_ui"), plugin(chat) },
                       { QStringLiteral("wallet_a"), plugin(wa) },
                       { QStringLiteral("wallet_b"), plugin(wb) } },
                     nullptr, nullptr);
}

void TestIntentBroker::buildOneProviderWorld(QTemporaryDir& root, IntentRegistry& registry)
{
    const QString chat = writeApp(root, QStringLiteral("chat"),
        R"({"uses":[{"intent":"wallet.send"}]})");
    const QString wa = writeApp(root, QStringLiteral("wa"),
        R"({"provides":[{"intent":"wallet.send"}]})");

    registry.rebuild({ { QStringLiteral("chat_ui"), plugin(chat) },
                       { QStringLiteral("wallet_a"), plugin(wa) } },
                     nullptr, nullptr);
}

void TestIntentBroker::testNotDeclaredShortCircuits()
{
    QTemporaryDir root;
    const QString chat = writeApp(root, QStringLiteral("chat"), R"({})");
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("chat_ui"), plugin(chat) } }, nullptr, nullptr);

    FakePresenter presenter;
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(500, 500, 50);

    FakeEndpoint chatEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"), {});
    spin(120);

    QCOMPARE(chatEndpoint.results.size(), 1);
    QCOMPARE(chatEndpoint.error(), QStringLiteral("not_declared"));
    // Nothing was loaded or shown: a missing declaration is decided locally.
    QVERIFY(presenter.ensureCalls.isEmpty());
    QVERIFY(presenter.presented.isEmpty());
}

void TestIntentBroker::testNoProviderIsUnavailableAfterFloor()
{
    QTemporaryDir root;
    const QString chat = writeApp(root, QStringLiteral("chat"),
        R"({"uses":[{"intent":"wallet.send"}]})");
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("chat_ui"), plugin(chat) } }, nullptr, nullptr);

    FakePresenter presenter;
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(2000, 2000, 200);

    FakeEndpoint chatEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);

    QElapsedTimer timer; timer.start();
    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"), {});
    spin(500);

    QCOMPARE(chatEndpoint.results.size(), 1);
    QCOMPARE(chatEndpoint.error(), QStringLiteral("unavailable"));
    // The floor defeats the trivial "an instant no means nothing is installed"
    // probe. It is a partial mitigation, not indistinguishability.
    QVERIFY2(timer.elapsed() >= 200, "unavailable was delivered before the error floor");
}

void TestIntentBroker::testRestrictedRequesterIsDeniedIndistinguishably()
{
    // A denied requester must not be able to tell "you are not allowed" from
    // "nothing provides this" — same code, same floor. Distinguishing the two
    // would hand an app an oracle for what the shell can do.
    QTemporaryDir root;
    const QString evil = writeApp(root, QStringLiteral("evil"),
        R"({"uses":[{"intent":"logos.packages.confirm_uninstall"}]})");
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("evil_ui"), plugin(evil) } }, nullptr, nullptr);

    // The shell really does provide it — so a leak here would be observable.
    registry.registerShellProvider(QStringLiteral("main_ui"),
        { QStringLiteral("logos.packages.confirm_uninstall") },
        QStringLiteral("Logos"), QString());
    registry.restrictIntentToRequesters(
        QStringLiteral("logos.packages.confirm_uninstall"),
        { QStringLiteral("package_manager_ui") });

    FakePresenter presenter;
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(2000, 2000, 200);

    FakeEndpoint evilEndpoint;
    broker.registerEndpoint(QStringLiteral("evil_ui"), &evilEndpoint);

    QElapsedTimer timer; timer.start();
    broker.submit(&evilEndpoint, QStringLiteral("req-1"),
                  QStringLiteral("logos.packages.confirm_uninstall"), {});
    spin(500);

    QCOMPARE(evilEndpoint.results.size(), 1);
    QCOMPARE(evilEndpoint.error(), QStringLiteral("unavailable"));
    QVERIFY2(timer.elapsed() >= 200, "denial was delivered before the error floor");
}

void TestIntentBroker::testHappyPathDispatchesAndRoutesBack()
{
    QTemporaryDir root;
    const QString chat = writeApp(root, QStringLiteral("chat"),
        R"({"uses":[{"intent":"packages.show"}]})");
    const QString pm = writeApp(root, QStringLiteral("pm"),
        R"({"provides":[{"intent":"packages.show"}]})");
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("chat_ui"), plugin(chat) },
                       { QStringLiteral("package_manager_ui"), plugin(pm) } },
                     nullptr, nullptr);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("package_manager_ui");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeEndpoint chatEndpoint, pmEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("package_manager_ui"), &pmEndpoint);

    // A single provider is confirmed like any other: the user is asked, then
    // the choice is resolved. Dispatching straight through used to make the
    // one-provider case the SILENT one, which is backwards — see startRequest.
    FakeChooser chooser;
    broker.setChooser(&chooser);

    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("packages.show"),
                  QVariantMap{ { QStringLiteral("package_id"), QStringLiteral("waku") } });
    spin(80);
    QCOMPARE(chooser.presented.size(), 1);
    QCOMPARE(chooser.lastProviders.size(), 1);
    broker.resolveChooser(chooser.presented.first(), QStringLiteral("package_manager_ui"));
    spin(80);

    QCOMPARE(pmEndpoint.requests.size(), 1);
    QCOMPARE(pmEndpoint.requests.first().intent, QStringLiteral("packages.show"));
    QCOMPARE(pmEndpoint.requests.first().requesterName, QStringLiteral("chat_ui"));
    QCOMPARE(pmEndpoint.requests.first().params.value(QStringLiteral("package_id")).toString(),
             QStringLiteral("waku"));
    QCOMPARE(presenter.presented, QStringList{ QStringLiteral("package_manager_ui") });

    broker.submitResponse(&pmEndpoint, pmEndpoint.requests.first().dispatchId,
                          true, QVariant(QVariantMap{ { QStringLiteral("shown"), true } }),
                          QString());
    spin(40);

    QCOMPARE(chatEndpoint.results.size(), 1);
    QCOMPARE(chatEndpoint.results.first().first, QStringLiteral("req-1"));   // its OWN id
    QVERIFY(chatEndpoint.ok());
}

void TestIntentBroker::testDispatchIdDiffersFromRequestId()
{
    QTemporaryDir root;
    IntentRegistry registry;
    buildOneProviderWorld(root, registry);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletA;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);

    submitAndConfirm(broker, chooser, &chatEndpoint, QStringLiteral("req-secret"),
                     QStringLiteral("wallet.send"), QStringLiteral("wallet_a"));

    QCOMPARE(walletA.requests.size(), 1);
    // The requester's id must never reach the provider. If these are ever the
    // same value, a provider can forge a response for a request it was not given.
    QVERIFY(walletA.requests.first().dispatchId != QStringLiteral("req-secret"));
    QVERIFY(!walletA.requests.first().dispatchId.isEmpty());
}

void TestIntentBroker::testSpoofedResponseIsIgnored()
{
    // THE CASE THIS FILE EXISTS FOR.
    QTemporaryDir root;
    IntentRegistry registry;
    buildOneProviderWorld(root, registry);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletA, walletB;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);
    broker.registerEndpoint(QStringLiteral("wallet_b"), &walletB);

    submitAndConfirm(broker, chooser, &chatEndpoint, QStringLiteral("req-1"),
                     QStringLiteral("wallet.send"), QStringLiteral("wallet_a"));
    QCOMPARE(walletA.requests.size(), 1);
    const QString realId = walletA.requests.first().dispatchId;

    // wallet_b was never given this request. Even holding the correct id, it
    // must not be able to answer for wallet_a.
    broker.submitResponse(&walletB, realId, true,
                          QVariant(QStringLiteral("stolen")), QString());
    spin(40);
    QCOMPARE(chatEndpoint.results.size(), 0);

    // A guessed id answers nothing either.
    broker.submitResponse(&walletB, QStringLiteral("made-up-id"), true, QVariant(), QString());
    spin(40);
    QCOMPARE(chatEndpoint.results.size(), 0);

    // The legitimate provider still works.
    broker.submitResponse(&walletA, realId, true, QVariant(QStringLiteral("real")), QString());
    spin(40);
    QCOMPARE(chatEndpoint.results.size(), 1);
    QCOMPARE(chatEndpoint.results.first().second.value(QStringLiteral("data")).toString(),
             QStringLiteral("real"));
}

void TestIntentBroker::testResponseFromWrongEndpointIsIgnored()
{
    // Same guard, stated as the requester trying to answer its own request.
    QTemporaryDir root;
    IntentRegistry registry;
    buildOneProviderWorld(root, registry);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletA;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);

    submitAndConfirm(broker, chooser, &chatEndpoint, QStringLiteral("req-1"),
                     QStringLiteral("wallet.send"), QStringLiteral("wallet_a"));

    broker.submitResponse(&chatEndpoint, walletA.requests.first().dispatchId,
                          true, QVariant(), QString());
    spin(40);
    QCOMPARE(chatEndpoint.results.size(), 0);
}

void TestIntentBroker::testSecondResponseIsIgnored()
{
    QTemporaryDir root;
    IntentRegistry registry;
    buildOneProviderWorld(root, registry);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletA;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);

    submitAndConfirm(broker, chooser, &chatEndpoint, QStringLiteral("req-1"),
                     QStringLiteral("wallet.send"), QStringLiteral("wallet_a"));
    const QString id = walletA.requests.first().dispatchId;

    broker.submitResponse(&walletA, id, true, QVariant(1), QString());
    broker.submitResponse(&walletA, id, true, QVariant(2), QString());
    spin(40);
    QCOMPARE(chatEndpoint.results.size(), 1);
}

void TestIntentBroker::testAmbiguousRaisesChooserAndPresentsNothing()
{
    QTemporaryDir root;
    IntentRegistry registry;
    buildTwoProviderWorld(root, registry);
    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a") << QStringLiteral("wallet_b");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);
    // A chooser must be mounted for an ambiguous request to be raised at all.
    FakeChooser chooser;
    broker.setChooser(&chooser);


    FakeEndpoint chatEndpoint, walletA, walletB;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);
    broker.registerEndpoint(QStringLiteral("wallet_b"), &walletB);

    QSignalSpy spy(&broker, &IntentBroker::chooserRequested);
    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"), {});
    spin(80);

    QCOMPARE(spy.count(), 1);
    const QVariantList providers = spy.first().at(3).toList();
    QCOMPARE(providers.size(), 2);
    QCOMPARE(providers.at(0).toMap().value(QStringLiteral("moduleName")).toString(),
             QStringLiteral("wallet_a"));   // sorted
    QCOMPARE(providers.at(1).toMap().value(QStringLiteral("moduleName")).toString(),
             QStringLiteral("wallet_b"));

    // NOTHING is presented or delivered until the user chooses. The chooser
    // returns a decision; it does not route.
    QVERIFY(presenter.presented.isEmpty());
    QVERIFY(walletA.requests.isEmpty());
    QVERIFY(walletB.requests.isEmpty());
    QVERIFY(chatEndpoint.results.isEmpty());
}

void TestIntentBroker::testChooserResolutionDispatches()
{
    QTemporaryDir root;
    IntentRegistry registry;
    buildTwoProviderWorld(root, registry);
    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_b");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);
    // A chooser must be mounted for an ambiguous request to be raised at all.
    FakeChooser chooser;
    broker.setChooser(&chooser);


    FakeEndpoint chatEndpoint, walletB;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_b"), &walletB);

    QSignalSpy spy(&broker, &IntentBroker::chooserRequested);
    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"), {});
    spin(80);
    const QString dispatchId = spy.first().at(0).toString();

    broker.resolveChooser(dispatchId, QStringLiteral("wallet_b"));
    spin(60);

    QCOMPARE(walletB.requests.size(), 1);
}

void TestIntentBroker::testChooserCancelIsCancelled()
{
    QTemporaryDir root;
    IntentRegistry registry;
    buildTwoProviderWorld(root, registry);
    FakePresenter presenter;
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);
    // A chooser must be mounted for an ambiguous request to be raised at all.
    FakeChooser chooser;
    broker.setChooser(&chooser);


    FakeEndpoint chatEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    QSignalSpy spy(&broker, &IntentBroker::chooserRequested);
    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"), {});
    spin(80);

    broker.cancelChooser(spy.first().at(0).toString());
    spin(40);

    QCOMPARE(chatEndpoint.results.size(), 1);
    // Indistinguishable from a provider-side cancel, deliberately.
    QCOMPARE(chatEndpoint.error(), QStringLiteral("cancelled"));
}

void TestIntentBroker::testActivationQueueLoadsOnce()
{
    // Two concurrent requests to one unloaded provider. Without the queue the
    // second hangs to its deadline: the shell's load notifications are
    // broadcasts carrying only a name, with no request correlation.
    QTemporaryDir root;
    IntentRegistry registry;
    buildOneProviderWorld(root, registry);

    FakePresenter presenter;   // wallet_a NOT loaded
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(5000, 5000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletA;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);

    // One dialog at a time, so the two confirmations are answered in turn. Both
    // requests are then in flight against a provider that is not loaded.
    submitAndConfirm(broker, chooser, &chatEndpoint, QStringLiteral("req-1"),
                     QStringLiteral("wallet.send"), QStringLiteral("wallet_a"));
    submitAndConfirm(broker, chooser, &chatEndpoint, QStringLiteral("req-2"),
                     QStringLiteral("wallet.send"), QStringLiteral("wallet_a"));

    QCOMPARE(presenter.ensureCalls.size(), 1);   // exactly once for both

    presenter.loaded << QStringLiteral("wallet_a");
    broker.onAppReady(QStringLiteral("wallet_a"));
    spin(60);
    QCOMPARE(walletA.requests.size(), 2);        // both drained
}

void TestIntentBroker::testProviderFreeTextBecomesFailed()
{
    QTemporaryDir root;
    IntentRegistry registry;
    buildOneProviderWorld(root, registry);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletA;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);
    submitAndConfirm(broker, chooser, &chatEndpoint, QStringLiteral("req-1"),
                     QStringLiteral("wallet.send"), QStringLiteral("wallet_a"));

    broker.submitResponse(&walletA, walletA.requests.first().dispatchId, false,
                          QVariant(), QStringLiteral("no such key in keystore"));
    spin(40);

    // Free text must never reach the requester's error path.
    QCOMPARE(chatEndpoint.error(), QStringLiteral("failed"));
}

void TestIntentBroker::testAmbiguousNeverReachesTheEnvelope()
{
    // A caller that could tell "two providers" from "none" can enumerate what
    // is installed, so the word must never appear in a delivered envelope.
    QTemporaryDir root;
    IntentRegistry registry;
    buildTwoProviderWorld(root, registry);
    FakePresenter presenter;
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);
    FakeChooser chooser;
    broker.setChooser(&chooser);


    FakeEndpoint chatEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    QSignalSpy spy(&broker, &IntentBroker::chooserRequested);
    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"), {});
    spin(80);
    broker.cancelChooser(spy.first().at(0).toString());
    spin(40);

    for (const auto& result : chatEndpoint.results) {
        for (const QVariant& value : result.second)
            QVERIFY(!value.toString().contains(QStringLiteral("ambiguous"), Qt::CaseInsensitive));
    }
}

void TestIntentBroker::testProviderDeathIsUnavailableNotFailed()
{
    QTemporaryDir root;
    IntentRegistry registry;
    buildOneProviderWorld(root, registry);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint;
    auto* walletA = new FakeEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), walletA);
    submitAndConfirm(broker, chooser, &chatEndpoint, QStringLiteral("req-1"),
                     QStringLiteral("wallet.send"), QStringLiteral("wallet_a"));

    broker.endpointDestroyed(walletA);
    delete walletA;
    spin(80);

    QCOMPARE(chatEndpoint.results.size(), 1);
    // "failed" would tell the requester the provider existed and was reached.
    QCOMPARE(chatEndpoint.error(), QStringLiteral("unavailable"));
}

void TestIntentBroker::testRequesterDeathDropsSilently()
{
    QTemporaryDir root;
    IntentRegistry registry;
    buildOneProviderWorld(root, registry);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    auto* chatEndpoint = new FakeEndpoint;
    FakeEndpoint walletA;
    broker.registerEndpoint(QStringLiteral("chat_ui"), chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);
    submitAndConfirm(broker, chooser, chatEndpoint, QStringLiteral("req-1"),
                     QStringLiteral("wallet.send"), QStringLiteral("wallet_a"));
    const QString id = walletA.requests.first().dispatchId;

    broker.endpointDestroyed(chatEndpoint);
    delete chatEndpoint;
    QCOMPARE(broker.pendingCount(), 0);

    // A late response for a dead requester is dropped as an unknown id — there
    // is no cancel symbol in the frozen surface, so this is the whole mechanism.
    broker.submitResponse(&walletA, id, true, QVariant(), QString());
    spin(40);   // must not crash
}


void TestIntentBroker::testAmbiguousWithNoChooserFailsRatherThanHangs()
{
    // AwaitingChoice has no deadline, on the reasoning that a human is looking
    // at a chooser. With no chooser connected that reasoning is false and the
    // request would hang for the life of the process.
    QTemporaryDir root;
    IntentRegistry registry;
    buildTwoProviderWorld(root, registry);
    FakePresenter presenter;
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeEndpoint chatEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);

    // The regression this pins: the shell ALWAYS connects chooserRequested in
    // order to re-emit it for QML, so an earlier version that counted receivers
    // on the broker's own signal saw 1 and waited forever. Model that here —
    // signal connected, but no dialog mounted.
    QSignalSpy chooserSpy(&broker, &IntentBroker::chooserRequested);
    FakeChooser chooser;
    chooser.mounted = false;
    broker.setChooser(&chooser);

    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"), {});
    spin(200);

    QCOMPARE(chatEndpoint.results.size(), 1);
    QCOMPARE(chatEndpoint.error(), QStringLiteral("unavailable"));
    QCOMPARE(broker.pendingCount(), 0);
    QCOMPARE(chooserSpy.count(), 0);   // never announced a choice nobody can make
}

void TestIntentBroker::testAmbiguousWithChooserAsksAndRoutesTheChoice()
{
    QTemporaryDir root;
    IntentRegistry registry;
    buildTwoProviderWorld(root, registry);
    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a") << QStringLiteral("wallet_b");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletA, walletB;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);
    broker.registerEndpoint(QStringLiteral("wallet_b"), &walletB);

    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"), {});
    spin(80);

    // Asked once, nothing dispatched, and the list is sorted by module name —
    // a non-deterministic chooser makes the two-provider E2E flaky.
    QCOMPARE(chooser.presented.size(), 1);
    QCOMPARE(chooser.lastProviders.size(), 2);
    QCOMPARE(chooser.lastProviders.at(0).toMap().value("moduleName").toString(),
             QStringLiteral("wallet_a"));
    QCOMPARE(walletA.requests.size(), 0);
    QCOMPARE(walletB.requests.size(), 0);

    // Pick the second one. Only it hears about the request.
    broker.resolveChooser(chooser.presented.first(), QStringLiteral("wallet_b"));
    spin(80);

    QCOMPARE(walletB.requests.size(), 1);
    QCOMPARE(walletA.requests.size(), 0);
    QCOMPARE(chooser.dismissed.size(), 1);
}

void TestIntentBroker::testShellProviderIsNeverLoadedOrPresented()
{
    // Regression: the shell is a provider for dispatch purposes but is NOT an
    // app. It has no widget and never appears in the presenter's loaded list,
    // so the generic path saw "not loaded" and called ensureAppLoaded("main_ui")
    // — which made basecamp try to load its own main UI plugin as a plugin and
    // hang inside MainUIPlugin::createWidget.
    QTemporaryDir root;
    IntentRegistry registry;
    const QString requester = writeApp(root, QStringLiteral("chat_ui"), R"({
        "name": "chat_ui", "type": "ui_qml",
        "uses": [ { "intent": "logos.repositories.manage" } ]
    })");
    QMap<QString, QVariantMap> plugins;
    plugins.insert(QStringLiteral("chat_ui"),
                   QVariantMap{{QStringLiteral("installDir"), requester},
                               {QStringLiteral("type"), QStringLiteral("ui_qml")}});
    registry.rebuild(plugins, [](const QString& n) { return n; },
                             [](const QString&) { return QString(); });
    registry.registerShellProvider(QStringLiteral("main_ui"),
                                   {QStringLiteral("logos.repositories.manage")},
                                   QStringLiteral("Logos"), QString());


    FakePresenter presenter;   // main_ui deliberately NOT in `loaded`
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(5000, 5000, 20);

    FakeEndpoint chatEndpoint, shellEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("main_ui"), &shellEndpoint);

    broker.submit(&chatEndpoint, QStringLiteral("req-1"),
                  QStringLiteral("logos.repositories.manage"), {});
    spin(80);

    // The whole point: the host is never loaded and never presented...
    QVERIFY(presenter.ensureCalls.isEmpty());
    QVERIFY(presenter.presented.isEmpty());
    // ...yet the request still reaches it.
    QCOMPARE(shellEndpoint.requests.size(), 1);
}

void TestIntentBroker::testInstallableProvidersNeverReachResolve()
{
    // THE LOAD-BEARING SEPARATION. A catalog entry says what COULD service an
    // intent once installed. resolve() must never return one: dispatching to a
    // package that is not on disk is impossible, not merely degraded, and one
    // merged table would make that an easy mistake to introduce later.
    QTemporaryDir root;
    IntentRegistry registry;
    registry.setInstallableProviders({{QStringLiteral("chat_ui"),
                                       {QStringLiteral("chat.group.open")}}});

    QCOMPARE(registry.installableProvidersFor(QStringLiteral("chat.group.open")),
             QStringList{QStringLiteral("chat_ui")});
    QCOMPARE(registry.resolve(QStringLiteral("chat.group.open")).status,
             IntentRegistry::None);
    QVERIFY(!registry.declaresProvide(QStringLiteral("chat_ui"),
                                      QStringLiteral("chat.group.open")));
}

void TestIntentBroker::testInstallableProviderIsOfferedNotDispatched()
{
    QTemporaryDir root;
    IntentRegistry registry;
    const QString requester = writeApp(root, QStringLiteral("chat_ui"), R"({
        "name": "chat_ui", "type": "ui_qml",
        "uses": [ { "intent": "wallet.send" } ]
    })");
    QMap<QString, QVariantMap> plugins;
    plugins.insert(QStringLiteral("chat_ui"),
                   QVariantMap{{QStringLiteral("installDir"), requester},
                               {QStringLiteral("type"), QStringLiteral("ui_qml")}});
    registry.rebuild(plugins, [](const QString& n) { return n; },
                             [](const QString&) { return QString(); });
    // Nothing INSTALLED provides wallet.send; the catalog knows a package.
    registry.setInstallableProviders({{QStringLiteral("wallet_x"),
                                       {QStringLiteral("wallet.send")}}});

    FakePresenter presenter;
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);
    FakeInstaller installer;
    broker.setInstaller(&installer);

    FakeEndpoint chatEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);

    broker.submit(&chatEndpoint, QStringLiteral("req-1"),
                  QStringLiteral("wallet.send"), {});
    spin(120);

    // Suggested to the USER, and answered to the APP in the same breath. The
    // offer is not a continuation: parking the request behind a download left
    // an invisible pending state that swallowed the user's next attempt, and
    // made time-to-answer track how long they deliberated.
    QCOMPARE(installer.offered.size(), 1);
    QCOMPARE(installer.lastCandidates, QStringList{QStringLiteral("wallet_x")});
    QCOMPARE(chatEndpoint.results.size(), 1);
    QCOMPARE(chatEndpoint.error(), QStringLiteral("unavailable"));
    QVERIFY(presenter.ensureCalls.isEmpty());   // nothing loaded on a catalog's word
}

void TestIntentBroker::testAnOfferIsIndistinguishableFromNoProviderAtAll()
{
    // Two runs of the same request: one where the catalog has a candidate and
    // one where it has nothing. The requester must not be able to tell them
    // apart — neither by the code nor by how long it took. If it could, any app
    // could enumerate what the user has NOT installed.
    auto run = [](bool catalogHasCandidate, QString* errorOut, qint64* elapsedOut) {
        QTemporaryDir root;
        IntentRegistry registry;
        const QString requester = writeApp(root, QStringLiteral("chat_ui"), R"({
            "name": "chat_ui", "type": "ui_qml",
            "uses": [ { "intent": "wallet.send" } ]
        })");
        QMap<QString, QVariantMap> plugins;
        plugins.insert(QStringLiteral("chat_ui"),
                       QVariantMap{{QStringLiteral("installDir"), requester},
                                   {QStringLiteral("type"), QStringLiteral("ui_qml")}});
        registry.rebuild(plugins, [](const QString& n) { return n; },
                                 [](const QString&) { return QString(); });
        if (catalogHasCandidate) {
            registry.setInstallableProviders({{QStringLiteral("wallet_x"),
                                               {QStringLiteral("wallet.send")}}});
        }

        FakePresenter presenter;
        IntentBroker broker(&registry, &presenter);
        broker.setTimeouts(1000, 1000, /*errorFloorMs=*/200);
        FakeInstaller installer;
        broker.setInstaller(&installer);

        FakeEndpoint chatEndpoint;
        broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);

        QElapsedTimer clock; clock.start();
        broker.submit(&chatEndpoint, QStringLiteral("req-1"),
                      QStringLiteral("wallet.send"), {});
        spin(400);

        QCOMPARE(chatEndpoint.results.size(), 1);
        *errorOut = chatEndpoint.error();
        *elapsedOut = clock.elapsed();
    };

    QString withCandidate, without;
    qint64 tWith = 0, tWithout = 0;
    run(true,  &withCandidate, &tWith);
    run(false, &without,       &tWithout);

    QCOMPARE(withCandidate, QStringLiteral("unavailable"));
    QCOMPARE(without,       QStringLiteral("unavailable"));

    // Both floored, neither waiting on a human. The old design answered only
    // after the user dismissed the dialog, so a slow `unavailable` announced
    // that an offer had been shown.
    QVERIFY(tWith >= 200);
    QVERIFY(tWith < 1000);
}

void TestIntentBroker::testSecondChoiceQueuesInsteadOfRepointingTheDialog()
{
    // THE CONSENT SWAP. There is one chooser instance, and openWith() on a
    // visible dialog silently replaces its contents — so a second request
    // arriving while the user is reaching for a provider row would be the one
    // they answer. An app controls its own request timing, which makes that an
    // attack rather than a nuisance.
    QTemporaryDir root;
    IntentRegistry registry;
    buildTwoProviderWorld(root, registry);
    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_a") << QStringLiteral("wallet_b");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(5000, 5000, 20);

    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletA, walletB;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_a"), &walletA);
    broker.registerEndpoint(QStringLiteral("wallet_b"), &walletB);

    broker.submit(&chatEndpoint, QStringLiteral("req-1"),
                  QStringLiteral("wallet.send"), {});
    spin(80);
    QCOMPARE(chooser.presented.size(), 1);
    const QString first = chooser.presented.first();

    // Second request while the first is on screen: must NOT be presented.
    broker.submit(&chatEndpoint, QStringLiteral("req-2"),
                  QStringLiteral("wallet.send"), {});
    spin(80);
    QCOMPARE(chooser.presented.size(), 1);          // still just the first
    QCOMPARE(chooser.presented.first(), first);     // and it was not repointed

    // Answering the first frees the dialog; the queued one gets its turn.
    broker.resolveChooser(first, QStringLiteral("wallet_a"));
    spin(120);

    QCOMPARE(chooser.presented.size(), 2);
    QVERIFY(chooser.presented.at(1) != first);
    QCOMPARE(walletA.requests.size(), 1);           // only the answered one ran
    QCOMPARE(walletB.requests.size(), 0);
}

void TestIntentBroker::testNonCanonicalParamsAreRefusedAsBadRequest()
{
    // The one check that needs no knowledge of the intent: can this value cross
    // between two QML engines at all? Nesting past the depth bound cannot, and
    // neither can a QObject* — which is the case that would hand one app a live
    // handle into another's engine.
    QTemporaryDir root;
    IntentRegistry registry;
    const QString requester = writeApp(root, QStringLiteral("chat_ui"), R"({
        "name": "chat_ui", "type": "ui_qml",
        "uses": [ { "intent": "wallet.send" } ]
    })");
    QMap<QString, QVariantMap> plugins;
    plugins.insert(QStringLiteral("chat_ui"),
                   QVariantMap{{QStringLiteral("installDir"), requester},
                               {QStringLiteral("type"), QStringLiteral("ui_qml")}});
    registry.rebuild(plugins, [](const QString& n) { return n; },
                             [](const QString&) { return QString(); });

    FakePresenter presenter;
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);

    FakeEndpoint chatEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);

    // Built from the inside out, comfortably past the depth bound of 8.
    QVariantMap deep;
    deep.insert(QStringLiteral("leaf"), QStringLiteral("x"));
    for (int i = 0; i < 12; ++i) {
        QVariantMap outer;
        outer.insert(QStringLiteral("n"), deep);
        deep = outer;
    }

    broker.submit(&chatEndpoint, QStringLiteral("req-1"),
                  QStringLiteral("wallet.send"), deep);
    spin(200);

    QCOMPARE(chatEndpoint.results.size(), 1);
    QCOMPARE(chatEndpoint.error(), QStringLiteral("bad_request"));

    // Refused before resolution — no provider was consulted, and none loaded.
    QVERIFY(presenter.ensureCalls.isEmpty());
    QVERIFY(presenter.presented.isEmpty());
}

void TestIntentBroker::testBadRequestIsFlooredLikeUnavailable()
{
    // A provider may also mint bad_request. If the broker answered its own
    // instantly, the delay would tell a caller whether a provider was ever
    // consulted — the existence oracle the merged `unavailable` prevents.
    QTemporaryDir root;
    IntentRegistry registry;
    const QString requester = writeApp(root, QStringLiteral("chat_ui"), R"({
        "name": "chat_ui", "type": "ui_qml",
        "uses": [ { "intent": "wallet.send" } ]
    })");
    QMap<QString, QVariantMap> plugins;
    plugins.insert(QStringLiteral("chat_ui"),
                   QVariantMap{{QStringLiteral("installDir"), requester},
                               {QStringLiteral("type"), QStringLiteral("ui_qml")}});
    registry.rebuild(plugins, [](const QString& n) { return n; },
                             [](const QString&) { return QString(); });

    FakePresenter presenter;
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, /*errorFloorMs=*/300);

    FakeEndpoint chatEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);

    QVariantMap tooLong;
    tooLong.insert(QStringLiteral("s"), QString(70000, QLatin1Char('x')));

    QElapsedTimer clock; clock.start();
    broker.submit(&chatEndpoint, QStringLiteral("req-1"),
                  QStringLiteral("wallet.send"), tooLong);
    spin(120);
    QCOMPARE(chatEndpoint.results.size(), 0);   // not answered yet — floored

    spin(300);
    QCOMPARE(chatEndpoint.results.size(), 1);
    QCOMPARE(chatEndpoint.error(), QStringLiteral("bad_request"));
    QVERIFY(clock.elapsed() >= 300);
}

// A provider that describes what wallet.send needs. This is the shape a real
// metadata.json carries under provides[].params.
static const char* kSpecProvider = R"({
    "provides": [ {
        "intent": "wallet.send",
        "params": [
            { "name": "to",     "type": "string", "required": true  },
            { "name": "amount", "type": "number", "required": true  },
            { "name": "memo",   "type": "string", "required": false }
        ]
    } ]
})";

void TestIntentBroker::testDeclaredParamsAreEnforcedAtDispatch()
{
    QTemporaryDir root;
    const QString chat = writeApp(root, QStringLiteral("chat"),
        R"({"uses":[{"intent":"wallet.send"}]})");
    const QString wallet = writeApp(root, QStringLiteral("wallet"), kSpecProvider);
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("chat_ui"), plugin(chat) },
                       { QStringLiteral("wallet_ui"), plugin(wallet) } },
                     nullptr, nullptr);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_ui");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);
    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_ui"), &walletEndpoint);

    // `amount` declared number, sent as a string. Wrong shape, not a wrong
    // world — the caller can fix this, which is why it is not `failed`.
    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"),
                  QVariantMap{ { QStringLiteral("to"), QStringLiteral("0xabc") },
                               { QStringLiteral("amount"), QStringLiteral("ten") } });
    spin(80);
    QCOMPARE(chooser.presented.size(), 1);
    broker.resolveChooser(chooser.presented.first(), QStringLiteral("wallet_ui"));
    spin(120);

    QCOMPARE(chatEndpoint.results.size(), 1);
    QCOMPARE(chatEndpoint.error(), QStringLiteral("bad_request"));
    // The provider never saw it. A payload it declared unusable should not
    // reach its handler at all.
    QCOMPARE(walletEndpoint.requests.size(), 0);

    // Same again, missing a required field entirely.
    chatEndpoint.results.clear();
    chooser.presented.clear();
    broker.submit(&chatEndpoint, QStringLiteral("req-2"), QStringLiteral("wallet.send"),
                  QVariantMap{ { QStringLiteral("amount"), 5 } });
    spin(80);
    broker.resolveChooser(chooser.presented.first(), QStringLiteral("wallet_ui"));
    spin(120);

    QCOMPARE(chatEndpoint.error(), QStringLiteral("bad_request"));
    QCOMPARE(walletEndpoint.requests.size(), 0);
}

void TestIntentBroker::testUndescribedAndExtraParamsStillDispatch()
{
    QTemporaryDir root;
    const QString chat = writeApp(root, QStringLiteral("chat"),
        R"({"uses":[{"intent":"wallet.send"}]})");
    const QString wallet = writeApp(root, QStringLiteral("wallet"), kSpecProvider);
    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("chat_ui"), plugin(chat) },
                       { QStringLiteral("wallet_ui"), plugin(wallet) } },
                     nullptr, nullptr);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("wallet_ui");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);
    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint chatEndpoint, walletEndpoint;
    broker.registerEndpoint(QStringLiteral("chat_ui"), &chatEndpoint);
    broker.registerEndpoint(QStringLiteral("wallet_ui"), &walletEndpoint);

    // Required fields present and correctly typed; `memo` omitted (optional),
    // and one field the provider never described. Extra fields pass: a caller
    // written against a NEWER provider must not be broken by an older
    // description, and providers may accept more than they list.
    broker.submit(&chatEndpoint, QStringLiteral("req-1"), QStringLiteral("wallet.send"),
                  QVariantMap{ { QStringLiteral("to"), QStringLiteral("0xabc") },
                               { QStringLiteral("amount"), 12.5 },
                               { QStringLiteral("chainId"), 1 } });
    spin(80);
    broker.resolveChooser(chooser.presented.first(), QStringLiteral("wallet_ui"));
    spin(80);

    QCOMPARE(walletEndpoint.requests.size(), 1);
    QCOMPARE(walletEndpoint.requests.first().params
                 .value(QStringLiteral("chainId")).toInt(), 1);

    // And a provider that describes nothing is not a provider that takes
    // nothing — an empty spec must not reject every payload.
    QTemporaryDir root2;
    const QString bare = writeApp(root2, QStringLiteral("bare"),
        R"({"provides":[{"intent":"wallet.send"}]})");
    IntentRegistry bareRegistry;
    bareRegistry.rebuild({ { QStringLiteral("chat_ui"), plugin(chat) },
                           { QStringLiteral("bare_ui"), plugin(bare) } },
                         nullptr, nullptr);
    FakePresenter presenter2;
    presenter2.loaded << QStringLiteral("bare_ui");
    IntentBroker broker2(&bareRegistry, &presenter2);
    broker2.setTimeouts(1000, 1000, 20);
    FakeChooser chooser2;
    broker2.setChooser(&chooser2);
    FakeEndpoint chat2, bareEndpoint;
    broker2.registerEndpoint(QStringLiteral("chat_ui"), &chat2);
    broker2.registerEndpoint(QStringLiteral("bare_ui"), &bareEndpoint);

    broker2.submit(&chat2, QStringLiteral("req-2"), QStringLiteral("wallet.send"),
                   QVariantMap{ { QStringLiteral("anything"), QStringLiteral("goes") } });
    spin(80);
    broker2.resolveChooser(chooser2.presented.first(), QStringLiteral("bare_ui"));
    spin(80);
    QCOMPARE(bareEndpoint.requests.size(), 1);
}

// Shared setup for the deadline cases: one requester, one provider, both
// declared, with a chooser standing by.
namespace {
struct TimeoutRig {
    QTemporaryDir root;
    IntentRegistry registry;
    FakePresenter  presenter;
    FakeChooser    chooser;
    FakeEndpoint   chat, wallet;
    std::unique_ptr<IntentBroker> broker;

    TimeoutRig()
    {
        const QString chatDir = writeApp(root, QStringLiteral("chat"),
            R"({"uses":[{"intent":"wallet.send"}]})");
        const QString walletDir = writeApp(root, QStringLiteral("wallet"),
            R"({"provides":[{"intent":"wallet.send"}]})");
        registry.rebuild({ { QStringLiteral("chat_ui"),   plugin(chatDir) },
                           { QStringLiteral("wallet_ui"), plugin(walletDir) } },
                         nullptr, nullptr);
        presenter.loaded << QStringLiteral("wallet_ui");
        broker = std::make_unique<IntentBroker>(&registry, &presenter);
        broker->setChooser(&chooser);
        broker->registerEndpoint(QStringLiteral("chat_ui"), &chat);
        broker->registerEndpoint(QStringLiteral("wallet_ui"), &wallet);
    }

    // Submit and answer the chooser, leaving the request Dispatched.
    void dispatch()
    {
        broker->submit(&chat, QStringLiteral("req-1"),
                       QStringLiteral("wallet.send"), {});
        spin(80);
        QVERIFY(!chooser.presented.isEmpty());
        broker->resolveChooser(chooser.presented.first(), QStringLiteral("wallet_ui"));
        spin(60);
    }
};
} // namespace

void TestIntentBroker::testProviderThatNeverAnswersTimesOut()
{
    // A provider WITH a handler is not killed by the short budget.
    //
    // This used to assert the opposite. The short budget existed to catch a
    // provider that never answers — but with a handler listening, the thing on
    // screen is that app's confirmation and the thing not answering is a
    // PERSON. Timing them out told the requester `timeout` while the provider,
    // told nothing (there is no withdraw symbol in the frozen surface),
    // completed the original send: a lost at-most-once guarantee, paid twice by
    // the user. AwaitingChoice already refuses to clock a human; this is the
    // same human, one dialog later.
    //
    // The backstop still bounds it, so a wedged provider cannot leak.
    TimeoutRig rig;
    rig.broker->setTimeouts(/*activationMs=*/2000, /*responseMs=*/250,
                            /*errorFloorMs=*/20);
    rig.dispatch();

    QCOMPARE(rig.wallet.requests.size(), 1);   // delivered, handler present
    QCOMPARE(rig.chat.results.size(), 0);

    spin(600);   // well past the short budget

    // Still waiting, deliberately.
    QCOMPARE(rig.chat.results.size(), 0);

    // And a late answer is still routed, rather than dropped as an unknown id.
    QVariantMap data{{QStringLiteral("done"), true}};
    rig.broker->submitResponse(&rig.wallet,
                               rig.wallet.requests.first().dispatchId,
                               true, data, QString());
    spin(60);
    QCOMPARE(rig.chat.results.size(), 1);
    QVERIFY(rig.chat.ok());
}

void TestIntentBroker::testProviderWithNoHandlerAlsoTimesOut()
{
    // Declared the capability, shipped no `onIntentRequested`. deliverRequest
    // reports zero receivers, which the broker logs but deliberately does NOT
    // fail fast on: a handler installed from an async Loader legitimately
    // arrives later, and the deadline is the honest bound on "it never showed
    // up". So this must reach the SAME `timeout`, not a distinct code.
    TimeoutRig rig;
    rig.wallet.receiverCount = 0;
    rig.broker->setTimeouts(2000, 250, 20);
    rig.dispatch();

    spin(500);

    QCOMPARE(rig.chat.results.size(), 1);
    QCOMPARE(rig.chat.error(), QStringLiteral("timeout"));
}

void TestIntentBroker::testActivationThatNeverCompletesEndsUnavailable()
{
    // The provider is chosen but never finishes loading — ensureAppLoaded is
    // called and onAppReady never arrives. That is `unavailable`, not
    // `timeout`: from the requester's side nothing was ever reached, and the
    // two codes must not be interchangeable or "a provider took the request
    // and went quiet" stops being distinguishable from "it was never there".
    TimeoutRig rig;
    rig.presenter.loaded.clear();              // nothing is loaded yet
    rig.broker->setTimeouts(/*activationMs=*/250, /*responseMs=*/2000,
                            /*errorFloorMs=*/20);
    rig.dispatch();

    QCOMPARE(rig.wallet.requests.size(), 0);   // never dispatched
    spin(500);

    QCOMPARE(rig.chat.results.size(), 1);
    QCOMPARE(rig.chat.error(), QStringLiteral("unavailable"));
}

void TestIntentBroker::testAnAppProvidingItsOwnIntentSkipsTheChooser()
{
    // An app may declare both `provides` and `uses` for one intent — reusing
    // its own capability for internal navigation. Routed through the chooser
    // that produces a dialog asking the user to pick an app to answer itself.
    QTemporaryDir root;
    const QString pm = writeApp(root, QStringLiteral("pm"), R"({
        "provides": [ { "intent": "packages.show" } ],
        "uses":     [ { "intent": "packages.show" } ]
    })");
    const QString other = writeApp(root, QStringLiteral("other"),
        R"({"provides":[{"intent":"packages.show"}]})");

    IntentRegistry registry;
    registry.rebuild({ { QStringLiteral("package_manager_ui"), plugin(pm) },
                       { QStringLiteral("other_ui"),           plugin(other) } },
                     nullptr, nullptr);

    FakePresenter presenter;
    presenter.loaded << QStringLiteral("package_manager_ui");
    IntentBroker broker(&registry, &presenter);
    broker.setTimeouts(1000, 1000, 20);
    FakeChooser chooser;
    broker.setChooser(&chooser);

    FakeEndpoint pmEndpoint, otherEndpoint;
    broker.registerEndpoint(QStringLiteral("package_manager_ui"), &pmEndpoint);
    broker.registerEndpoint(QStringLiteral("other_ui"), &otherEndpoint);

    broker.submit(&pmEndpoint, QStringLiteral("req-1"),
                  QStringLiteral("packages.show"), {});
    spin(120);

    // Straight to itself. Note ANOTHER app also provides this — so it is the
    // self-match that short-circuits, not a lack of alternatives.
    QVERIFY(chooser.presented.isEmpty());
    QCOMPARE(pmEndpoint.requests.size(), 1);
    QCOMPARE(otherEndpoint.requests.size(), 0);
}

QTEST_MAIN(TestIntentBroker)
#include "intent_broker_test.moc"
