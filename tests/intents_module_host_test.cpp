// srcdeps: intents/IntentsModuleHost.cpp
//
// Basecamp as intents_module's presenter, against a fake module: what it sends,
// when a click opens a request, how work items reach the shell, how an app's
// backend is shown as its app and started when picked, when it takes the person
// back, and how it survives the module restarting.

#include "intents/IntentsModuleHost.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

namespace {

struct Sent {
    QString method;
    QVariantList args;
};

// Answers like intents_module: one claim per instance, presenter calls only
// with that instance's secret, and whatever work the test queued.
class FakeModule {
public:
    QList<Sent> sent;
    QJsonArray work;
    QString secret;   // the current instance's; empty until claimed
    int claims = 0;
    int handles = 0;
    int refuseSetInstalled = 0;   // this many set_installed fail, as a call that times out
    int refuseDeclareShell = 0;   // and declare_shell

    // A new instance: unclaimed, and the old secret means nothing to it.
    void restart() { secret.clear(); }

    IntentsModuleHost::Call call()
    {
        return [this](const QString& method, const QVariantList& args) {
            sent.append({method, args});
            const QJsonObject refused{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("refused")}};
            QJsonObject r{{QStringLiteral("ok"), true}};
            if (method == QStringLiteral("claim")) {
                if (!secret.isEmpty()) return reply(refused);
                secret = QStringLiteral("s%1").arg(++claims);
                r[QStringLiteral("secret")] = secret;
                return reply(r);
            }
            if (secret.isEmpty() || args.value(0).toString() != secret) return reply(refused);
            if (method == QStringLiteral("set_installed") && refuseSetInstalled > 0) {
                --refuseSetInstalled;
                return QString();
            }
            if (method == QStringLiteral("declare_shell") && refuseDeclareShell > 0) {
                --refuseDeclareShell;
                return QString();
            }
            if (method == QStringLiteral("shell_request")) r[QStringLiteral("handle")] = QStringLiteral("h%1").arg(++handles);
            if (method == QStringLiteral("next_work")) {
                r[QStringLiteral("items")] = work;
                work = QJsonArray();
            }
            return reply(r);
        };
    }

    QList<Sent> calls(const QString& method) const
    {
        QList<Sent> out;
        for (const Sent& s : sent)
            if (s.method == method) out.append(s);
        return out;
    }

    QStringList methods() const
    {
        QStringList out;
        for (const Sent& s : sent) out << s.method;
        return out;
    }

    QJsonObject lastAnswer() const
    {
        const QList<Sent> a = calls(QStringLiteral("answer"));
        return a.isEmpty() ? QJsonObject()
                           : QJsonDocument::fromJson(a.last().args.value(2).toString().toUtf8()).object();
    }

    // The pending requests the host opened.
    QStringList opened() const
    {
        QStringList out;
        for (const Sent& s : calls(QStringLiteral("answer"))) {
            const QJsonObject a = QJsonDocument::fromJson(s.args.value(2).toString().toUtf8()).object();
            if (a.value(QStringLiteral("action")).toString() == QStringLiteral("open"))
                out << s.args.value(1).toString();
        }
        return out;
    }

private:
    static QString reply(const QJsonObject& o)
    {
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
};

class FakePresenter : public IntentPresenter {
public:
    QSet<QString> loaded;
    QString frontmost;
    bool dialog = false;
    QStringList ensured;
    QStringList presented;

    bool isAppLoaded(const QString& app) const override { return loaded.contains(app); }
    void ensureAppLoaded(const QString& app) override { ensured << app; }
    void presentApp(const QString& app) override
    {
        presented << app;
        frontmost = app;
    }
    bool isAppFrontmost(const QString& app) const override { return frontmost == app; }
    bool anyDialogOpen() const override { return dialog; }
};

// What the fake had been sent by the time a line was logged.
class LogWatch {
public:
    explicit LogWatch(const FakeModule& m)
    {
        s_module = &m;
        s_lines.clear();
        m_previous = qInstallMessageHandler(&LogWatch::handler);
    }
    ~LogWatch()
    {
        qInstallMessageHandler(m_previous);
        s_module = nullptr;
    }

    // -1 when no line had `text`.
    qsizetype sentWhenLogged(const QString& text) const
    {
        for (const auto& [line, sent] : s_lines)
            if (line.contains(text)) return sent;
        return -1;
    }

private:
    static void handler(QtMsgType, const QMessageLogContext&, const QString& line)
    {
        s_lines.append({line, s_module ? s_module->sent.size() : -1});
    }

    static inline const FakeModule* s_module = nullptr;
    static inline QList<QPair<QString, qsizetype>> s_lines;
    QtMessageHandler m_previous = nullptr;
};

// UIPluginManager::appOfBackend, for the apps these tests install.
QString appOfBackend(const QString& module)
{
    const QStringList apps{QStringLiteral("wallet"), QStringLiteral("signer"), QStringLiteral("package_manager_ui")};
    if (!module.endsWith(QStringLiteral("_backend"))) return QString();
    const QString app = module.chopped(8);
    return apps.contains(app) ? app : QString();
}

// How the QML inspector clicks: an event sent, not one from the window system.
void sentClick(QWidget* view)
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(1, 1), view->mapToGlobal(QPointF(1, 1)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view, &press);
}

QJsonObject item(const char* kind, const char* id, QJsonObject fields = {})
{
    fields[QStringLiteral("kind")] = QLatin1String(kind);
    fields[QStringLiteral("id")] = QLatin1String(id);
    return fields;
}

QJsonObject pending(const char* id, const QString& requester = QStringLiteral("wallet_backend"))
{
    return item("pending", id, {{QStringLiteral("intent"), QStringLiteral("signing.approve")},
                                {QStringLiteral("requester"), requester}});
}

QJsonObject chooseSigner(const char* id)
{
    return item("choose", id, {
        {QStringLiteral("intent"), QStringLiteral("signing.approve")},
        {QStringLiteral("requester"), QStringLiteral("wallet_backend")},
        {QStringLiteral("providers"), QJsonArray{QJsonObject{
             {QStringLiteral("module"), QStringLiteral("signer_backend")},
             {QStringLiteral("displayName"), QStringLiteral("Signer")}}}}});
}

// The signer answered the wallet; both are apps, through their backends.
QJsonObject completed(const char* dispatch, const QJsonObject& fields = {})
{
    QJsonObject done{{QStringLiteral("dispatch"), QLatin1String(dispatch)},
                     {QStringLiteral("requester"), QStringLiteral("wallet_backend")},
                     {QStringLiteral("provider"), QStringLiteral("signer_backend")},
                     {QStringLiteral("handoff"), false},
                     {QStringLiteral("by"), QStringLiteral("provider")}};
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) done.insert(it.key(), it.value());
    return item("completed", "wc", done);
}

QJsonObject present(const char* dispatch, const QString& provider = QStringLiteral("signer_backend"))
{
    return item("present", "wp", {{QStringLiteral("provider"), provider},
                                  {QStringLiteral("dispatch"), QLatin1String(dispatch)}});
}

// Claimed, and the module holds the shell and what is installed.
void makeReady(IntentsModuleHost& host)
{
    host.claim();
    host.declareShell(QJsonObject{{QStringLiteral("name"), QStringLiteral("main_ui")}});
    host.setInstalled(QJsonArray());
}

void backendsUp(IntentsModuleHost& host)
{
    host.registerBackend(QStringLiteral("wallet"), QStringLiteral("wallet_backend"));
    host.registerBackend(QStringLiteral("signer"), QStringLiteral("signer_backend"));
}

} // namespace

class TestIntentsModuleHost : public QObject {
    Q_OBJECT

private slots:
    void claimsWithNoOptionsAndSendsTheSecret();
    void basecampsOwnRequestGoesThroughTheModule();
    void basecampsOwnSingleOptionChoiceIsAnsweredWithoutADialog();
    void basecampsOwnSingleOptionChoiceStartsTheAppFirst();
    void aRestartFailsBasecampsOwnRequestsInFlight();
    void anInstallOfferNamesTheIntent();
    void whichAppABackendIsStaysInBasecamp();
    void nothingIsSentBeforeTheClaim();
    void basecampAsksNothingBeforeTheModuleHoldsItsShell();
    void aRefusedSyncIsSentAgainUntilTaken();
    void anAttemptEndsAtItsFirstFailedCall();
    void aDeclarationWhileARetryIsDueWaitsForIt();
    void retriesBackOff();
    void nothingIsPulledBeforeTheModuleIsReady();
    void aModuleHandingOutWorkIsSyncedAtOnce();
    void workARestartThatWasNotOneFoundWaitsUntilReady();
    void aRestartIsAnnouncedOnceTheModuleIsReady();
    void theSameInstalledPackagesAreSentOnce();
    void anAppUpgradedInPlaceIsSentAgain();
    void aManifestRewrittenWithItsSizeAndTimeIsSentAgain();
    void aManifestThatIsAFifoIsNotRead();

    void immediateOpensEveryRequest();
    void whatShipsWithBasecampOpensAtOnce();
    void anyOtherRequestOpensOnlyOnAClick();
    void interactiveWaitsEvenForWhatShipsWithBasecamp();
    void aDoubleClickOpensOneRequest();
    void aRestartForgetsUnspentClicks();
    void interactiveLeavesARequestWithNoClickPending();
    void aClickInTheAppsScreenOpensItsBackendsNextRequest();
    void oneClickOpensOneRequest();
    void aStaleClickOpensNothing();
    void aClickInAnotherAppOpensNothingHere();
    void aModuleWithNoScreenIsNotOpenedByAClick();
    void synthesizedInputCountsOnlyWhenAllowed();
    void aStoppedBackendLosesItsClick();
    void aRebuiltViewKeepsOpeningRequests();

    void aChoiceReachesTheShellAndItsAnswerTheModule();
    void aChoiceIsAnsweredOnce();
    void aDismissedChoiceCloses();
    void anAppsBackendIsShownAndPickedAsItsApp();
    void aChosenProviderIsStartedBeforeTheAnswer();
    void aPickItDidNotOfferStartsNothing();
    void aChosenAppIsAnsweredOnceItsBackendIsUp();
    void aChosenAppThatCannotProvideIsAnsweredAnyway();
    void aClosedAppIsStartedAgainWhenPicked();
    void picksOfOneAppShareItsStart();
    void aPickStopsWaitingForAnAppThatNeverComes();
    void aRestartDropsPicksWaitingForAnApp();

    void presentRaisesTheBackendsApp();
    void presentForAModuleWithNoScreenRaisesNothing();
    void theShellsOwnIntentGoesToTheShellProvider();
    void aShellIntentNobodyTakesIsUnavailable();
    void aWithdrawnShellDeliveryIsNotAnswered();
    void aDismissedShellDeliveryEndsItsDialog();
    void aRestartEndsTheShellsDeliveries();

    void anAnsweredRequestTakesThePersonBack();
    void autoReturnKeepsItsGuards();
    void autoReturnWaitsOutTheDwellFloor();
    void aBurstOfReturnsPausesThem();
    void aRequestThatTookNobodyAnywhereBringsNobodyBack();
    void aDelayedReturnIsDroppedByARestart();

    void aRestartedModuleIsClaimedAndToldEverythingAgain();
    void aRestartKeepsWhichAppEachBackendIs();
    void aRestartThatWasNotOneKeepsTheClaim();
    void aStoppedModuleIsLoadedAgainAtMostThreeTimes();
    void nothingIsLoadedAgainOnceShutdownBegan();
    void pastTheLastReloadNothingIsAskedOfIt();
    void aFailedReloadLeavesNothingToAsk();
    void aFailedReloadSaysWhetherItIsTriedAgain();
    void aModuleThatCantBeClaimedAgainIsGivenUp();
};

// When to interrupt the person is Basecamp's own business: the module is not told.
void TestIntentsModuleHost::claimsWithNoOptionsAndSendsTheSecret()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    QVERIFY(host.claim());
    QVERIFY(host.claim());
    const QList<Sent> claims = m.calls(QStringLiteral("claim"));
    QCOMPARE(claims.size(), 1);
    QVERIFY(claims[0].args.isEmpty());

    host.declareShell(QJsonObject{{QStringLiteral("name"), QStringLiteral("main_ui")}});
    QCOMPARE(m.calls(QStringLiteral("declare_shell")).value(0).args.value(0).toString(), QStringLiteral("s1"));
}

void TestIntentsModuleHost::basecampsOwnRequestGoesThroughTheModule()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    QVariantMap got;
    QVERIFY(host.shellRequest(QStringLiteral("wallet.send"), {{QStringLiteral("to"), QStringLiteral("x")}},
                              /*asLink=*/true, [&](const QVariantMap& envelope) { got = envelope; }));
    const QList<Sent> sent = m.calls(QStringLiteral("shell_request"));
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent[0].args.value(1).toString(), QStringLiteral("wallet.send"));
    QCOMPARE(QJsonDocument::fromJson(sent[0].args.value(2).toString().toUtf8()).object()
                 .value(QStringLiteral("to")).toString(), QStringLiteral("x"));
    QCOMPARE(sent[0].args.value(3).toBool(), true);

    // Its result comes back as a work item, to whoever asked.
    m.work.append(item("result", "w1", {{QStringLiteral("handle"), QStringLiteral("h1")},
                                        {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), false},
                                                                               {QStringLiteral("error"), QStringLiteral("cancelled")}}}}));
    host.pullWork();
    QCOMPARE(got.value(QStringLiteral("error")).toString(), QStringLiteral("cancelled"));
}

void TestIntentsModuleHost::basecampsOwnSingleOptionChoiceIsAnsweredWithoutADialog()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    QSignalSpy shown(&host, &IntentsModuleHost::chooserRequested);
    makeReady(host);
    const QJsonObject pm{{QStringLiteral("module"), QStringLiteral("package_manager_ui_backend")}};
    const QJsonArray one{pm};
    const QJsonArray two{pm, QJsonObject{{QStringLiteral("module"), QStringLiteral("other_ui_backend")}}};

    m.work.append(item("choose", "own", {{QStringLiteral("requester"), QStringLiteral("main_ui")},
                                         {QStringLiteral("providers"), one}}));
    host.pullWork();
    QCOMPARE(shown.count(), 0);
    QCOMPARE(m.lastAnswer().value(QStringLiteral("action")).toString(), QStringLiteral("choose"));
    QCOMPARE(m.lastAnswer().value(QStringLiteral("provider")).toString(), QStringLiteral("package_manager_ui_backend"));

    // A link is always shown, and so is a real choice.
    m.work.append(item("choose", "link", {{QStringLiteral("requester"), QStringLiteral("logos_link")},
                                          {QStringLiteral("providers"), one}}));
    m.work.append(item("choose", "both", {{QStringLiteral("requester"), QStringLiteral("main_ui")},
                                          {QStringLiteral("providers"), two}}));
    host.pullWork();
    QCOMPARE(shown.count(), 2);
}

// As a pick in the chooser: answered once the app's backend is up.
void TestIntentsModuleHost::basecampsOwnSingleOptionChoiceStartsTheAppFirst()
{
    FakeModule m;
    FakePresenter p;
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setAppOfModule(appOfBackend);
    makeReady(host);
    m.work.append(item("choose", "own", {
        {QStringLiteral("requester"), QStringLiteral("main_ui")},
        {QStringLiteral("providers"),
         QJsonArray{QJsonObject{{QStringLiteral("module"), QStringLiteral("package_manager_ui_backend")}}}}}));
    host.pullWork();
    QCOMPARE(p.ensured, QStringList{QStringLiteral("package_manager_ui")});
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());

    host.registerBackend(QStringLiteral("package_manager_ui"), QStringLiteral("package_manager_ui_backend"));
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 1);
    QCOMPARE(m.lastAnswer().value(QStringLiteral("provider")).toString(), QStringLiteral("package_manager_ui_backend"));
}

void TestIntentsModuleHost::aRestartFailsBasecampsOwnRequestsInFlight()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    QVariantMap got;
    QVERIFY(host.shellRequest(QStringLiteral("packages.show"), {}, false,
                              [&](const QVariantMap& envelope) { got = envelope; }));
    m.restart();
    host.restarted();
    QCOMPARE(got.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(got.value(QStringLiteral("error")).toString(), QStringLiteral("failed"));
}

// The catalogue stays in Basecamp: the module names the intent, the shell finds candidates.
void TestIntentsModuleHost::anInstallOfferNamesTheIntent()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    QSignalSpy offered(&host, &IntentsModuleHost::installOffered);
    m.work.append(item("offer_install", "o1", QJsonObject{{QStringLiteral("intent"), QStringLiteral("share.text")}}));
    host.pullWork();
    QCOMPARE(offered.size(), 1);
    QCOMPARE(offered[0].value(0).toString(), QStringLiteral("share.text"));
}

// The module knows modules: an app's backend coming and going is Basecamp's.
void TestIntentsModuleHost::whichAppABackendIsStaysInBasecamp()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    QVERIFY(host.claim());
    host.registerBackend(QStringLiteral("wallet"), QStringLiteral("wallet_backend"));
    host.appReady(QStringLiteral("wallet"));
    host.unregisterBackend(QStringLiteral("wallet"));
    QCOMPARE(m.methods(), QStringList{QStringLiteral("claim")});
}

void TestIntentsModuleHost::nothingIsSentBeforeTheClaim()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setInstalled(QJsonArray());
    host.declareShell(QJsonObject());
    host.pullWork();
    QVERIFY(!host.shellRequest(QStringLiteral("packages.show"), {}, false));
    QVERIFY(m.sent.isEmpty());
}

// For development: every request pops up.
void TestIntentsModuleHost::immediateOpensEveryRequest()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("immediate"));
    makeReady(host);
    m.work.append(pending("w1"));
    m.work.append(pending("w2", QStringLiteral("chain_module")));
    host.pullWork();
    QCOMPARE(m.opened(), (QStringList{QStringLiteral("w1"), QStringLiteral("w2")}));
}

void TestIntentsModuleHost::interactiveLeavesARequestWithNoClickPending()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    backendsUp(host);

    m.work.append(pending("w1"));
    host.pullWork();
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());
}

void TestIntentsModuleHost::aClickInTheAppsScreenOpensItsBackendsNextRequest()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    backendsUp(host);

    QTest::mousePress(&view, Qt::LeftButton);
    m.work.append(pending("w1"));
    host.pullWork();
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w1")});

    QTest::keyClick(&view, Qt::Key_Return);
    m.work.append(pending("w2"));
    host.pullWork();
    QCOMPARE(m.opened(), (QStringList{QStringLiteral("w1"), QStringLiteral("w2")}));
}

void TestIntentsModuleHost::oneClickOpensOneRequest()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    backendsUp(host);

    QTest::mousePress(&view, Qt::LeftButton);
    m.work.append(pending("w1"));
    m.work.append(pending("w2"));
    host.pullWork();
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w1")});

    QTest::mousePress(&view, Qt::LeftButton);
    m.work.append(pending("w3"));
    host.pullWork();
    QCOMPARE(m.opened(), (QStringList{QStringLiteral("w1"), QStringLiteral("w3")}));
}

void TestIntentsModuleHost::aStaleClickOpensNothing()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    host.setInputWindowMs(20);
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    backendsUp(host);

    QTest::mousePress(&view, Qt::LeftButton);
    QTest::qWait(60);
    m.work.append(pending("w1"));
    host.pullWork();
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());
}

void TestIntentsModuleHost::aClickInAnotherAppOpensNothingHere()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    makeReady(host);
    QWidget wallet, signer;
    host.watchView(QStringLiteral("wallet"), &wallet);
    host.watchView(QStringLiteral("signer"), &signer);
    backendsUp(host);

    QTest::mousePress(&signer, Qt::LeftButton);
    m.work.append(pending("w1"));
    m.work.append(pending("w2", QStringLiteral("signer_backend")));
    host.pullWork();
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w2")});
}

void TestIntentsModuleHost::aModuleWithNoScreenIsNotOpenedByAClick()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    host.setAppOfModule(appOfBackend);
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);

    QTest::mousePress(&view, Qt::LeftButton);
    m.work.append(pending("w1", QStringLiteral("chain_module")));
    m.work.append(pending("w2"));
    host.pullWork();
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w2")});
}

void TestIntentsModuleHost::synthesizedInputCountsOnlyWhenAllowed()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    backendsUp(host);

    sentClick(&view);
    m.work.append(pending("w1"));
    host.pullWork();

    // QTest's events are marked spontaneous, like the window system's.
    QTest::mousePress(&view, Qt::LeftButton);
    m.work.append(pending("w2"));
    host.pullWork();

    host.setAcceptSynthesizedInput(true);
    sentClick(&view);
    m.work.append(pending("w3"));
    host.pullWork();
    QCOMPARE(m.opened(), (QStringList{QStringLiteral("w2"), QStringLiteral("w3")}));
}

// A click from before the backend stopped is not for the one that came up after.
void TestIntentsModuleHost::aStoppedBackendLosesItsClick()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    backendsUp(host);

    QTest::mousePress(&view, Qt::LeftButton);
    host.unregisterBackend(QStringLiteral("wallet"));
    host.registerBackend(QStringLiteral("wallet"), QStringLiteral("wallet_backend"));
    m.work.append(pending("w1"));
    host.pullWork();
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());
}

// A hot reload replaces the widget: the new one counts after the old one goes.
void TestIntentsModuleHost::aRebuiltViewKeepsOpeningRequests()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    makeReady(host);
    auto* first = new QWidget;
    QWidget second;
    host.watchView(QStringLiteral("wallet"), first);
    host.watchView(QStringLiteral("wallet"), &second);
    delete first;
    backendsUp(host);

    QTest::mousePress(&second, Qt::LeftButton);
    m.work.append(pending("w1"));
    host.pullWork();
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w1")});
}

void TestIntentsModuleHost::aChoiceReachesTheShellAndItsAnswerTheModule()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    QSignalSpy shown(&host, &IntentsModuleHost::chooserRequested);

    m.work.append(item("choose", "w1", {
        {QStringLiteral("intent"), QStringLiteral("signing.approve")},
        {QStringLiteral("requester"), QStringLiteral("wallet")},
        {QStringLiteral("providers"), QJsonArray{QJsonObject{
             {QStringLiteral("module"), QStringLiteral("signer")},
             {QStringLiteral("displayName"), QStringLiteral("Signer")},
             {QStringLiteral("iconSource"), QString()}}}}}));
    host.pullWork();

    QCOMPARE(shown.size(), 1);
    QCOMPARE(shown[0][0].toString(), QStringLiteral("w1"));
    QCOMPARE(shown[0][2].toString(), QStringLiteral("wallet"));
    const QVariantMap provider = shown[0][3].toList().value(0).toMap();
    QCOMPARE(provider.value(QStringLiteral("moduleName")).toString(), QStringLiteral("signer"));
    QCOMPARE(provider.value(QStringLiteral("displayName")).toString(), QStringLiteral("Signer"));
    QVERIFY(host.ownsChoice(QStringLiteral("w1")));

    host.choose(QStringLiteral("w1"), QStringLiteral("signer"));
    const QList<Sent> answers = m.calls(QStringLiteral("answer"));
    QCOMPARE(answers.size(), 1);
    QCOMPARE(answers[0].args[1].toString(), QStringLiteral("w1"));
    QCOMPARE(m.lastAnswer().value(QStringLiteral("action")).toString(), QStringLiteral("choose"));
    QCOMPARE(m.lastAnswer().value(QStringLiteral("provider")).toString(), QStringLiteral("signer"));
}

void TestIntentsModuleHost::aChoiceIsAnsweredOnce()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    m.work.append(item("choose", "w1"));
    host.pullWork();

    host.cancelChoice(QStringLiteral("w1"));
    host.choose(QStringLiteral("w1"), QStringLiteral("signer"));
    host.choose(QStringLiteral("unknown"), QStringLiteral("signer"));
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 1);
    QCOMPARE(m.lastAnswer().value(QStringLiteral("action")).toString(), QStringLiteral("cancel"));
    QVERIFY(!host.ownsChoice(QStringLiteral("w1")));
}

void TestIntentsModuleHost::aDismissedChoiceCloses()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    QSignalSpy dismissed(&host, &IntentsModuleHost::chooserDismissed);
    m.work.append(item("choose", "w1"));
    host.pullWork();
    m.work.append(item("dismiss", "w2", {{QStringLiteral("work"), QStringLiteral("w1")}}));
    host.pullWork();

    QCOMPARE(dismissed.size(), 1);
    QCOMPARE(dismissed[0][0].toString(), QStringLiteral("w1"));
    host.choose(QStringLiteral("w1"), QStringLiteral("signer"));
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());
}

// The person sees and picks apps; the module hears their backends.
void TestIntentsModuleHost::anAppsBackendIsShownAndPickedAsItsApp()
{
    FakeModule m;
    FakePresenter p;
    p.loaded = {QStringLiteral("wallet"), QStringLiteral("signer")};
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    makeReady(host);
    backendsUp(host);
    QSignalSpy shown(&host, &IntentsModuleHost::chooserRequested);

    m.work.append(item("choose", "w1", {
        {QStringLiteral("intent"), QStringLiteral("signing.approve")},
        {QStringLiteral("requester"), QStringLiteral("wallet_backend")},
        {QStringLiteral("providers"), QJsonArray{
             QJsonObject{{QStringLiteral("module"), QStringLiteral("signer_backend")}},
             QJsonObject{{QStringLiteral("module"), QStringLiteral("cli_signer")}}}}}));
    host.pullWork();

    QCOMPARE(shown.size(), 1);
    QCOMPARE(shown[0][2].toString(), QStringLiteral("wallet"));
    const QVariantList providers = shown[0][3].toList();
    QCOMPARE(providers.value(0).toMap().value(QStringLiteral("moduleName")).toString(), QStringLiteral("signer"));
    QCOMPARE(providers.value(1).toMap().value(QStringLiteral("moduleName")).toString(), QStringLiteral("cli_signer"));

    host.choose(QStringLiteral("w1"), QStringLiteral("signer"));
    QCOMPARE(m.lastAnswer().value(QStringLiteral("provider")).toString(), QStringLiteral("signer_backend"));
}

// A core module the person chose is started before the module delivers to it.
void TestIntentsModuleHost::aChosenProviderIsStartedBeforeTheAnswer()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    QString started;
    std::function<void()> finish;
    host.setProviderStarter([&](const QString& provider, std::function<void()> done) {
        started = provider;
        finish = std::move(done);
    });
    m.work.append(item("choose", "w1", {{QStringLiteral("providers"), QJsonArray{QJsonObject{
                                            {QStringLiteral("module"), QStringLiteral("echo_provider")}}}}}));
    host.pullWork();

    host.choose(QStringLiteral("w1"), QStringLiteral("echo_provider"));
    QCOMPARE(started, QStringLiteral("echo_provider"));
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());
    finish();
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 1);
    QCOMPARE(m.lastAnswer().value(QStringLiteral("provider")).toString(), QStringLiteral("echo_provider"));
}

// A name the chooser didn't offer is no pick: nothing starts, and the request
// is cancelled rather than left waiting for an answer the module would refuse.
void TestIntentsModuleHost::aPickItDidNotOfferStartsNothing()
{
    FakeModule m;
    FakePresenter p;
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setAppOfModule(&appOfBackend);
    makeReady(host);
    QStringList started;
    host.setProviderStarter([&](const QString& provider, std::function<void()> done) {
        started << provider;
        done();
    });
    m.work.append(chooseSigner("w1"));
    host.pullWork();

    host.choose(QStringLiteral("w1"), QStringLiteral("chain_module"));
    QVERIFY(started.isEmpty());
    QVERIFY(p.ensured.isEmpty());
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 1);
    QCOMPARE(m.lastAnswer().value(QStringLiteral("action")).toString(), QStringLiteral("cancel"));
    QVERIFY(!host.ownsChoice(QStringLiteral("w1")));
}

// An app's backend starts with its app: the pick waits for it to register.
void TestIntentsModuleHost::aChosenAppIsAnsweredOnceItsBackendIsUp()
{
    FakeModule m;
    FakePresenter p;
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setAppOfModule(appOfBackend);
    QStringList starterAsked;
    host.setProviderStarter([&](const QString& provider, std::function<void()> done) {
        starterAsked << provider;
        done();
    });
    makeReady(host);
    QSignalSpy shown(&host, &IntentsModuleHost::chooserRequested);

    m.work.append(chooseSigner("w1"));
    host.pullWork();
    QCOMPARE(shown[0][3].toList().value(0).toMap().value(QStringLiteral("moduleName")).toString(),
             QStringLiteral("signer"));
    host.choose(QStringLiteral("w1"), QStringLiteral("signer"));
    QCOMPARE(p.ensured, QStringList{QStringLiteral("signer")});
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());

    host.registerBackend(QStringLiteral("signer"), QStringLiteral("signer_backend"));
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 1);
    QCOMPARE(m.lastAnswer().value(QStringLiteral("provider")).toString(), QStringLiteral("signer_backend"));
    p.loaded.insert(QStringLiteral("signer"));
    host.appReady(QStringLiteral("signer"));
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 1);

    // Running already: answered at once.
    m.work.append(chooseSigner("w2"));
    host.pullWork();
    host.choose(QStringLiteral("w2"), QStringLiteral("signer"));
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 2);
    QCOMPARE(p.ensured.size(), 1);
    QVERIFY(starterAsked.isEmpty());
}

// Answered all the same: delivering to a backend that isn't running fails as
// `unavailable` in the module, which is the truth.
void TestIntentsModuleHost::aChosenAppThatCannotProvideIsAnsweredAnyway()
{
    struct Case {
        const char* name;
        bool loaded;
        std::function<void(IntentsModuleHost&)> then;
    };
    const QList<Case> cases = {
        {"up already, with no backend", true, [](IntentsModuleHost&) {}},
        {"came up with no backend", false, [](IntentsModuleHost& h) { h.appReady(QStringLiteral("signer")); }},
        {"did not load", false, [](IntentsModuleHost& h) { h.appLoadFailed(QStringLiteral("signer")); }},
    };
    for (const Case& c : cases) {
        FakeModule m;
        FakePresenter p;
        if (c.loaded) p.loaded = {QStringLiteral("signer")};
        IntentsModuleHost host(m.call());
        host.setPresenter(&p);
        host.setAppOfModule(appOfBackend);
        makeReady(host);
        m.work.append(chooseSigner("w1"));
        host.pullWork();

        host.choose(QStringLiteral("w1"), QStringLiteral("signer"));
        c.then(host);
        QVERIFY2(m.calls(QStringLiteral("answer")).size() == 1, c.name);
        QVERIFY2(m.lastAnswer().value(QStringLiteral("provider")).toString() == QStringLiteral("signer_backend"),
                 c.name);
        QVERIFY2(p.ensured.size() == (c.loaded ? 0 : 1), c.name);
    }
}

// Closing an app takes its backend with it: a pick starts the app again.
void TestIntentsModuleHost::aClosedAppIsStartedAgainWhenPicked()
{
    FakeModule m;
    FakePresenter p;
    p.loaded = {QStringLiteral("signer")};
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setAppOfModule(appOfBackend);
    makeReady(host);
    host.registerBackend(QStringLiteral("signer"), QStringLiteral("signer_backend"));
    p.loaded.clear();

    m.work.append(chooseSigner("w1"));
    host.pullWork();
    host.choose(QStringLiteral("w1"), QStringLiteral("signer"));
    QCOMPARE(p.ensured, QStringList{QStringLiteral("signer")});
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());

    host.registerBackend(QStringLiteral("signer"), QStringLiteral("signer_backend"));
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 1);
}

void TestIntentsModuleHost::picksOfOneAppShareItsStart()
{
    FakeModule m;
    FakePresenter p;
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setAppOfModule(appOfBackend);
    makeReady(host);
    m.work.append(chooseSigner("w1"));
    m.work.append(chooseSigner("w2"));
    host.pullWork();

    host.choose(QStringLiteral("w1"), QStringLiteral("signer"));
    host.choose(QStringLiteral("w2"), QStringLiteral("signer"));
    QCOMPARE(p.ensured, QStringList{QStringLiteral("signer")});
    host.registerBackend(QStringLiteral("signer"), QStringLiteral("signer_backend"));
    QStringList answered;
    for (const Sent& s : m.calls(QStringLiteral("answer"))) answered << s.args.value(1).toString();
    QCOMPARE(answered, (QStringList{QStringLiteral("w1"), QStringLiteral("w2")}));
}

void TestIntentsModuleHost::aPickStopsWaitingForAnAppThatNeverComes()
{
    FakeModule m;
    FakePresenter p;
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setAppOfModule(appOfBackend);
    host.setStartTimeoutMs(20);
    makeReady(host);
    m.work.append(chooseSigner("w1"));
    host.pullWork();

    host.choose(QStringLiteral("w1"), QStringLiteral("signer"));
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(m.calls(QStringLiteral("answer")).size(), 1, 1000);
    host.registerBackend(QStringLiteral("signer"), QStringLiteral("signer_backend"));
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 1);
}

// The pick was the old instance's: the new one is not answered for it.
void TestIntentsModuleHost::aRestartDropsPicksWaitingForAnApp()
{
    FakeModule m;
    FakePresenter p;
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setAppOfModule(appOfBackend);
    makeReady(host);
    m.work.append(chooseSigner("w1"));
    host.pullWork();
    host.choose(QStringLiteral("w1"), QStringLiteral("signer"));

    m.restart();
    host.restarted();
    host.registerBackend(QStringLiteral("signer"), QStringLiteral("signer_backend"));
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());
}

void TestIntentsModuleHost::presentRaisesTheBackendsApp()
{
    FakeModule m;
    FakePresenter p;
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    makeReady(host);
    backendsUp(host);
    m.work.append(present("d1"));
    host.pullWork();
    QCOMPARE(p.presented, QStringList{QStringLiteral("signer")});
}

// A terminal or a headless module answered: nobody was taken anywhere, or back.
void TestIntentsModuleHost::presentForAModuleWithNoScreenRaisesNothing()
{
    FakeModule m;
    FakePresenter p;
    p.loaded = {QStringLiteral("wallet")};
    p.frontmost = QStringLiteral("wallet");
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setReturnDwellFloorMs(0);
    makeReady(host);
    backendsUp(host);
    m.work.append(present("d1", QStringLiteral("cli_signer")));
    m.work.append(completed("d1", {{QStringLiteral("provider"), QStringLiteral("cli_signer")}}));
    host.pullWork();
    QVERIFY(p.presented.isEmpty());
}

// PackageCoordinator knows the app, not its backend: the confirmations are Package Manager's.
void TestIntentsModuleHost::theShellsOwnIntentGoesToTheShellProvider()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    host.registerBackend(QStringLiteral("package_manager_ui"), QStringLiteral("package_manager_ui_backend"));
    QStringList got;
    QVariantMap gotParams;
    host.setShellProvider([&](const QString& id, const QString& intent, const QVariantMap& params,
                              const QString& requester) {
        got = {id, intent, requester};
        gotParams = params;
        return 1;
    });
    m.work.append(item("deliver", "w1", {
        {QStringLiteral("intent"), QStringLiteral("basecamp.packages.confirm_install")},
        {QStringLiteral("requester"), QStringLiteral("package_manager_ui_backend")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("chat_ui")}}}}));
    host.pullWork();

    QCOMPARE(got, (QStringList{QStringLiteral("w1"), QStringLiteral("basecamp.packages.confirm_install"),
                               QStringLiteral("package_manager_ui")}));
    QCOMPARE(gotParams.value(QStringLiteral("name")).toString(), QStringLiteral("chat_ui"));
    QVERIFY(host.ownsShellDelivery(QStringLiteral("w1")));
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());

    QVERIFY(host.respondShell(QStringLiteral("w1"), true,
                              QVariantMap{{QStringLiteral("plan"), QStringLiteral("p")}}, QString()));
    QCOMPARE(m.lastAnswer().value(QStringLiteral("action")).toString(), QStringLiteral("respond"));
    QCOMPARE(m.lastAnswer().value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(m.lastAnswer().value(QStringLiteral("data")).toObject().value(QStringLiteral("plan")).toString(),
             QStringLiteral("p"));
    QVERIFY(!host.respondShell(QStringLiteral("w1"), false, QVariant(), QStringLiteral("cancelled")));
    QCOMPARE(m.calls(QStringLiteral("answer")).size(), 1);
}

void TestIntentsModuleHost::aShellIntentNobodyTakesIsUnavailable()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    host.setShellProvider([](const QString&, const QString&, const QVariantMap&, const QString&) { return 0; });
    m.work.append(item("deliver", "w1", {{QStringLiteral("intent"), QStringLiteral("basecamp.settings.open")}}));
    host.pullWork();
    QCOMPARE(m.lastAnswer().value(QStringLiteral("action")).toString(), QStringLiteral("respond"));
    QCOMPARE(m.lastAnswer().value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(m.lastAnswer().value(QStringLiteral("error")).toString(), QStringLiteral("unavailable"));
    QVERIFY(!host.ownsShellDelivery(QStringLiteral("w1")));
}

// The request ended first (the requester cancelled, a deadline): the answer
// reports that nobody heard it, so a confirmation can fall back.
void TestIntentsModuleHost::aWithdrawnShellDeliveryIsNotAnswered()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    host.setShellProvider([](const QString&, const QString&, const QVariantMap&, const QString&) { return 1; });
    m.work.append(item("deliver", "w1", {{QStringLiteral("intent"), QStringLiteral("basecamp.packages.confirm_uninstall")}}));
    host.pullWork();
    m.work.append(item("dismiss", "w2", {{QStringLiteral("work"), QStringLiteral("w1")}}));
    host.pullWork();

    QVERIFY(!host.ownsShellDelivery(QStringLiteral("w1")));
    QVERIFY(!host.respondShell(QStringLiteral("w1"), true, QVariant(), QString()));
    QVERIFY(m.calls(QStringLiteral("answer")).isEmpty());
}

void TestIntentsModuleHost::anAnsweredRequestTakesThePersonBack()
{
    FakeModule m;
    FakePresenter p;
    p.loaded = {QStringLiteral("wallet"), QStringLiteral("signer")};
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setReturnDwellFloorMs(0);
    makeReady(host);
    backendsUp(host);
    m.work.append(present("d1"));
    m.work.append(completed("d1"));
    host.pullWork();
    QCOMPARE(p.presented, (QStringList{QStringLiteral("signer"), QStringLiteral("wallet")}));
}

void TestIntentsModuleHost::autoReturnKeepsItsGuards()
{
    struct Case {
        const char* name;
        std::function<void(FakePresenter&)> setup;
        QJsonObject done;
    };
    const QList<Case> cases = {
        {"a deadline", [](FakePresenter&) {}, completed("d1", {{QStringLiteral("by"), QStringLiteral("deadline")}})},
        {"a hand-off", [](FakePresenter&) {}, completed("d1", {{QStringLiteral("handoff"), true}})},
        {"moved on", [](FakePresenter& p) { p.frontmost = QStringLiteral("other"); }, completed("d1")},
        {"a dialog up", [](FakePresenter& p) { p.dialog = true; }, completed("d1")},
        {"requester gone", [](FakePresenter& p) { p.loaded.remove(QStringLiteral("wallet")); }, completed("d1")},
        {"a requester with no screen", [](FakePresenter&) {},
         completed("d1", {{QStringLiteral("requester"), QStringLiteral("chain_module")}})},
    };
    for (const Case& c : cases) {
        FakeModule m;
        FakePresenter p;
        p.loaded = {QStringLiteral("wallet"), QStringLiteral("signer")};
        IntentsModuleHost host(m.call());
        host.setPresenter(&p);
        host.setReturnDwellFloorMs(0);
        makeReady(host);
        backendsUp(host);
        m.work.append(present("d1"));
        host.pullWork();
        c.setup(p);
        m.work.append(c.done);
        host.pullWork();
        QVERIFY2(p.presented == QStringList{QStringLiteral("signer")}, c.name);
    }
}

void TestIntentsModuleHost::autoReturnWaitsOutTheDwellFloor()
{
    FakeModule m;
    FakePresenter p;
    p.loaded = {QStringLiteral("wallet"), QStringLiteral("signer")};
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setReturnDwellFloorMs(100);
    makeReady(host);
    backendsUp(host);

    m.work.append(present("d1"));
    m.work.append(completed("d1"));
    host.pullWork();
    QVERIFY(!p.presented.contains(QStringLiteral("wallet")));
    QTRY_VERIFY_WITH_TIMEOUT(p.presented.contains(QStringLiteral("wallet")), 1000);

    // Gone elsewhere during the floor: not owed a ride back.
    p.presented.clear();
    m.work.append(present("d2"));
    m.work.append(completed("d2"));
    host.pullWork();
    p.frontmost = QStringLiteral("other");
    QTest::qWait(200);
    QVERIFY(!p.presented.contains(QStringLiteral("wallet")));
}

void TestIntentsModuleHost::aBurstOfReturnsPausesThem()
{
    FakeModule m;
    FakePresenter p;
    p.loaded = {QStringLiteral("wallet"), QStringLiteral("signer")};
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setReturnDwellFloorMs(0);
    makeReady(host);
    backendsUp(host);
    const auto roundTrip = [&]() {
        m.work.append(present("d"));
        m.work.append(completed("d"));
        host.pullWork();
    };

    for (int i = 0; i < 7; ++i) roundTrip();
    QCOMPARE(p.presented.count(QStringLiteral("wallet")), 6);

    // Re-armed once things have been quiet for the cooldown.
    host.setReturnBreakerCooldownMs(0);
    roundTrip();
    QCOMPARE(p.presented.count(QStringLiteral("wallet")), 7);
}

void TestIntentsModuleHost::aRestartedModuleIsClaimedAndToldEverythingAgain()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    QSignalSpy dismissed(&host, &IntentsModuleHost::chooserDismissed);
    host.claim();
    host.declareShell(QJsonObject{{QStringLiteral("name"), QStringLiteral("main_ui")}});
    host.setInstalled(QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("wallet")}}});
    backendsUp(host);
    m.work.append(item("choose", "w1"));
    host.pullWork();

    m.restart();
    m.sent.clear();
    qsizetype sentWhenClaimedAgain = -1;
    {
        LogWatch log(m);
        host.restarted();
        sentWhenClaimedAgain = log.sentWhenLogged(QStringLiteral("claimed again"));
    }

    QCOMPARE(m.methods(), (QStringList{QStringLiteral("next_work"), QStringLiteral("claim"),
                                      QStringLiteral("declare_shell"), QStringLiteral("set_installed"),
                                      QStringLiteral("next_work")}));
    QVERIFY(host.isClaimed());
    QCOMPARE(m.calls(QStringLiteral("set_installed"))[0].args.value(0).toString(), QStringLiteral("s2"));
    // Said only once the new instance knows what the old one did: whoever
    // waits for it may ask at once.
    QCOMPARE(sentWhenClaimedAgain, qsizetype(4));
    QCOMPARE(dismissed.size(), 1);
    QVERIFY(!host.ownsChoice(QStringLiteral("w1")));
}

// Apps' backends kept running through it.
void TestIntentsModuleHost::aRestartKeepsWhichAppEachBackendIs()
{
    FakeModule m;
    FakePresenter p;
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setAttention(QStringLiteral("interactive"));
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    backendsUp(host);

    m.restart();
    host.restarted();
    QTest::mousePress(&view, Qt::LeftButton);
    m.work.append(pending("w1"));
    m.work.append(present("d1"));
    host.pullWork();
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w1")});
    QCOMPARE(p.presented, QStringList{QStringLiteral("signer")});
}

void TestIntentsModuleHost::aRestartThatWasNotOneKeepsTheClaim()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    m.work.append(item("choose", "w1"));
    m.sent.clear();
    host.restarted();
    QCOMPARE(m.methods(), QStringList{QStringLiteral("next_work")});
    QVERIFY(host.ownsChoice(QStringLiteral("w1")));
    QCOMPARE(m.claims, 1);
}

void TestIntentsModuleHost::aStoppedModuleIsLoadedAgainAtMostThreeTimes()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.claim();
    QVERIFY(!host.takeReload(/*moduleRunning=*/true));

    QVERIFY(host.takeReload(false));
    QVERIFY2(!host.takeReload(false), "a second reload while one is under way");
    m.restart();
    host.reloadFinished(true);
    QCOMPARE(m.claims, 2);

    QVERIFY(host.takeReload(false));
    host.reloadFinished(false);
    QVERIFY(host.takeReload(false));
    host.reloadFinished(false);
    QVERIFY2(!host.takeReload(false), "a fourth reload");
}

// Quitting stops every module: none of that is a crash to recover from.
void TestIntentsModuleHost::nothingIsLoadedAgainOnceShutdownBegan()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.claim();
    QVERIFY(host.takeReload(false));
    host.shutdown();
    m.restart();
    host.reloadFinished(true);
    QCOMPARE(m.claims, 1);
    QVERIFY(!host.takeReload(false));
}

// Past its last reload the module is off: what was open fails, and nothing is
// asked of a module that isn't there, so Basecamp's own requests fall back.
void TestIntentsModuleHost::pastTheLastReloadNothingIsAskedOfIt()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    for (int i = 0; i < 3; ++i) {
        QVERIFY(host.takeReload(false));
        m.restart();
        host.reloadFinished(true);
    }
    QVariantMap got;
    QVERIFY(host.shellRequest(QStringLiteral("packages.show"), {}, false,
                              [&](const QVariantMap& envelope) { got = envelope; }));
    QVERIFY(!host.takeReload(false));
    QVERIFY(!host.isClaimed());
    QCOMPARE(got.value(QStringLiteral("error")).toString(), QStringLiteral("failed"));
    const qsizetype sent = m.sent.size();
    QVERIFY(!host.shellRequest(QStringLiteral("packages.show"), {}, false));
    host.pullWork();
    QCOMPARE(m.sent.size(), sent);
}

// A reload that fails leaves no module either.
void TestIntentsModuleHost::aFailedReloadLeavesNothingToAsk()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    QVariantMap got;
    QVERIFY(host.shellRequest(QStringLiteral("packages.show"), {}, false,
                              [&](const QVariantMap& envelope) { got = envelope; }));
    QVERIFY(host.takeReload(false));
    m.restart();
    host.reloadFinished(false);
    QVERIFY(!host.isClaimed());
    QCOMPARE(got.value(QStringLiteral("error")).toString(), QStringLiteral("failed"));
    QVERIFY(!host.shellRequest(QStringLiteral("packages.show"), {}, false));
}

// A failed reload is tried again on the next change to what runs, within the
// three; the log says which.
void TestIntentsModuleHost::aFailedReloadSaysWhetherItIsTriedAgain()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    LogWatch log(m);
    QVERIFY(host.takeReload(false));
    host.reloadFinished(false);
    QVERIFY(log.sentWhenLogged(QStringLiteral("tried again when the modules next change")) >= 0);
    QCOMPARE(log.sentWhenLogged(QStringLiteral("off for this session")), qsizetype(-1));
    for (int i = 0; i < 2; ++i) {
        QVERIFY(host.takeReload(false));
        host.reloadFinished(false);
    }
    QVERIFY(log.sentWhenLogged(QStringLiteral("off for this session")) >= 0);
    QVERIFY(!host.takeReload(false));
}

// Basecamp's own requests need the module to know the shell and what is
// installed, or they would find no one who answers.
void TestIntentsModuleHost::basecampAsksNothingBeforeTheModuleHoldsItsShell()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    QVERIFY(host.claim());
    QVERIFY(!host.shellRequest(QStringLiteral("packages.show"), {}, false));
    host.declareShell(QJsonObject{{QStringLiteral("name"), QStringLiteral("main_ui")}});
    QVERIFY(!host.shellRequest(QStringLiteral("packages.show"), {}, false));
    host.setInstalled(QJsonArray());
    QVERIFY(host.isReady());
    QVERIFY(host.shellRequest(QStringLiteral("packages.show"), {}, false));
}

// A call that failed leaves the module behind: it is sent again until taken.
void TestIntentsModuleHost::aRefusedSyncIsSentAgainUntilTaken()
{
    FakeModule m;
    m.refuseSetInstalled = 2;
    IntentsModuleHost host(m.call());
    host.setSyncRetryMs(10);
    makeReady(host);
    QVERIFY(!host.isReady());
    QTRY_VERIFY_WITH_TIMEOUT(host.isReady(), 2000);
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 3);
    QCOMPARE(m.calls(QStringLiteral("declare_shell")).size(), 1);
}

// Each call holds the UI until it times out: one that failed ends the attempt.
void TestIntentsModuleHost::anAttemptEndsAtItsFirstFailedCall()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setSyncRetryMs(10);
    makeReady(host);
    m.restart();
    m.refuseDeclareShell = 1;
    m.sent.clear();
    host.restarted();
    QCOMPARE(m.methods(), (QStringList{QStringLiteral("next_work"), QStringLiteral("claim"),
                                      QStringLiteral("declare_shell")}));
    QTRY_VERIFY_WITH_TIMEOUT(host.isReady(), 2000);
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 1);
}

// What changes while a retry is due goes with it, not sooner.
void TestIntentsModuleHost::aDeclarationWhileARetryIsDueWaitsForIt()
{
    FakeModule m;
    m.refuseDeclareShell = 1;
    IntentsModuleHost host(m.call());
    host.setSyncRetryMs(50);
    QVERIFY(host.claim());
    host.declareShell(QJsonObject{{QStringLiteral("name"), QStringLiteral("main_ui")}});
    host.setInstalled(QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("wallet")}}});
    host.declareShell(QJsonObject{{QStringLiteral("name"), QStringLiteral("main_ui")}});
    QCOMPARE(m.methods(), (QStringList{QStringLiteral("claim"), QStringLiteral("declare_shell")}));
    QTRY_VERIFY_WITH_TIMEOUT(host.isReady(), 2000);
    QCOMPARE(m.calls(QStringLiteral("declare_shell")).size(), 2);
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 1);
}

// A module that never answers is asked less and less often, and never given up on.
void TestIntentsModuleHost::retriesBackOff()
{
    FakeModule m;
    m.refuseDeclareShell = 1000;
    IntentsModuleHost host(m.call());
    host.setSyncRetryMs(10, 80);
    makeReady(host);
    QTest::qWait(400);
    // 10, 20, 40, then every 80 ms: 7 or 8 by now; every 10 ms would be 40.
    const qsizetype asked = m.calls(QStringLiteral("declare_shell")).size();
    QVERIFY2(asked >= 4 && asked <= 10, qPrintable(QString::number(asked)));
    QVERIFY(m.calls(QStringLiteral("set_installed")).isEmpty());
    m.refuseDeclareShell = 0;
    QTRY_VERIFY_WITH_TIMEOUT(host.isReady(), 2000);
}

// Until the module holds the shell and what is installed, what it would hand
// out was made without them: it waits there, and is pulled once it does.
void TestIntentsModuleHost::nothingIsPulledBeforeTheModuleIsReady()
{
    FakeModule m;
    m.refuseSetInstalled = 2;   // the retry work_available brings fails too
    IntentsModuleHost host(m.call());
    host.setSyncRetryMs(10);
    host.setAttention(QStringLiteral("immediate"));
    makeReady(host);
    m.work.append(pending("w1"));
    host.pullWork();
    QVERIFY(m.calls(QStringLiteral("next_work")).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(m.opened(), QStringList{QStringLiteral("w1")}, 2000);
    QCOMPARE(m.methods().indexOf(QStringLiteral("next_work")),
             m.methods().lastIndexOf(QStringLiteral("set_installed")) + 1);
}

// A module that hands out work answers again: what it didn't take goes at
// once, while the click behind that work still counts, not at the backoff's
// next turn, up to a minute later.
void TestIntentsModuleHost::aModuleHandingOutWorkIsSyncedAtOnce()
{
    FakeModule m;
    m.refuseSetInstalled = 1;
    IntentsModuleHost host(m.call());
    host.setSyncRetryMs(60 * 1000);
    host.setAttention(QStringLiteral("interactive"));
    host.setAppOfModule(&appOfBackend);
    makeReady(host);
    QVERIFY(!host.isReady());
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    QTest::mousePress(&view, Qt::LeftButton);
    m.work.append(pending("w1"));
    host.pullWork();
    QVERIFY(host.isReady());
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 2);
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w1")});
}

// The same instance after all: what its probe pulled waits for readiness too.
void TestIntentsModuleHost::workARestartThatWasNotOneFoundWaitsUntilReady()
{
    FakeModule m;
    m.refuseSetInstalled = 1;
    IntentsModuleHost host(m.call());
    host.setSyncRetryMs(50);
    host.setAttention(QStringLiteral("immediate"));
    makeReady(host);
    m.work.append(pending("w1"));
    host.restarted();
    QVERIFY(m.opened().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(m.opened(), QStringList{QStringLiteral("w1")}, 2000);
    QCOMPARE(m.claims, 1);
}

void TestIntentsModuleHost::aRestartIsAnnouncedOnceTheModuleIsReady()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setSyncRetryMs(10);
    makeReady(host);
    m.restart();
    m.refuseSetInstalled = 1;
    m.sent.clear();
    LogWatch log(m);
    host.restarted();
    QCOMPARE(log.sentWhenLogged(QStringLiteral("claimed again")), qsizetype(-1));
    QTRY_VERIFY_WITH_TIMEOUT(host.isReady(), 2000);
    // next_work, claim, declare_shell, set_installed (failed), set_installed.
    QCOMPARE(log.sentWhenLogged(QStringLiteral("claimed again")), qsizetype(5));
}

void TestIntentsModuleHost::theSameInstalledPackagesAreSentOnce()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    const QJsonArray installed{QJsonObject{{QStringLiteral("name"), QStringLiteral("wallet")}}};
    host.setInstalled(installed);
    host.setInstalled(installed);
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 2);
    // A new instance holds nothing yet: it is told again.
    m.restart();
    host.restarted();
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 3);
}

// The module reads what a package declares from its metadata.json: an upgrade
// in place changes that and nothing in the package's entry.
void TestIntentsModuleHost::anAppUpgradedInPlaceIsSentAgain()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString manifest = dir.filePath(QStringLiteral("metadata.json"));
    const auto write = [&](const QByteArray& text, const QDateTime& at) {
        QFile f(manifest);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text);
        QVERIFY(f.flush());
        QVERIFY(f.setFileTime(at, QFileDevice::FileModificationTime));
    };
    const QDateTime installedAt = QDateTime::currentDateTime().addSecs(-60);
    write(R"({"name":"wallet","version":"1.0.0","provides":["wallet.send"]})", installedAt);

    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    const QJsonArray installed{QJsonObject{{QStringLiteral("name"), QStringLiteral("wallet")},
                                           {QStringLiteral("installDir"), dir.path()}}};
    host.setInstalled(installed);
    host.setInstalled(installed);
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 2);

    write(R"({"name":"wallet","version":"1.1.0","provides":["wallet.send","wallet.sign"]})",
          installedAt.addSecs(30));
    host.setInstalled(installed);
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 3);
}

// By its bytes, not its stat: a copy that keeps the file's time (every file in
// the nix store is dated 1970) can change what a package declares without
// changing its size.
void TestIntentsModuleHost::aManifestRewrittenWithItsSizeAndTimeIsSentAgain()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString manifest = dir.filePath(QStringLiteral("metadata.json"));
    const QDateTime storeTime = QDateTime::fromSecsSinceEpoch(1);
    const auto write = [&](const QByteArray& text) {
        QFile f(manifest);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text);
        QVERIFY(f.flush());
        QVERIFY(f.setFileTime(storeTime, QFileDevice::FileModificationTime));
    };
    const QByteArray before = R"({"name":"wallet","version":"0.1.0","provides":["wallet.send"]})";
    const QByteArray after = R"({"name":"wallet","version":"0.1.1","provides":["wallet.sign"]})";
    QCOMPARE(before.size(), after.size());
    write(before);

    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    const QJsonArray installed{QJsonObject{{QStringLiteral("name"), QStringLiteral("wallet")},
                                           {QStringLiteral("installDir"), dir.path()}}};
    host.setInstalled(installed);
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 2);
    write(after);
    QCOMPARE(QFileInfo(manifest).lastModified(), storeTime);
    host.setInstalled(installed);
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 3);
}

// A package's metadata.json may be a FIFO: reading it would wait forever for
// a writer, so it isn't read, and the list still goes out.
void TestIntentsModuleHost::aManifestThatIsAFifoIsNotRead()
{
#ifdef Q_OS_WIN
    QSKIP("no FIFOs on Windows");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QCOMPARE(::mkfifo(QFile::encodeName(dir.filePath(QStringLiteral("metadata.json"))).constData(), 0600), 0);
    FakeModule m;
    IntentsModuleHost host(m.call());
    makeReady(host);
    host.setInstalled(QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("wallet")},
                                             {QStringLiteral("installDir"), dir.path()}}});
    QCOMPARE(m.calls(QStringLiteral("set_installed")).size(), 2);
#endif
}

// By default what ships with Basecamp opens at once; anything else waits for a
// click in its app's screen, so a core module's request waits for good.
void TestIntentsModuleHost::whatShipsWithBasecampOpensAtOnce()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setRequesterBundled([](const QString& module) { return module == QStringLiteral("wallet_backend"); });
    makeReady(host);
    m.work.append(pending("w1"));
    m.work.append(pending("w2", QStringLiteral("stranger_module")));
    host.pullWork();
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w1")});
}

void TestIntentsModuleHost::anyOtherRequestOpensOnlyOnAClick()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setRequesterBundled([](const QString&) { return false; });
    host.setAppOfModule(&appOfBackend);
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    m.work.append(pending("w1"));
    host.pullWork();
    QVERIFY(m.opened().isEmpty());
    QTest::mousePress(&view, Qt::LeftButton);
    m.work.append(pending("w2"));
    host.pullWork();
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w2")});
}

void TestIntentsModuleHost::interactiveWaitsEvenForWhatShipsWithBasecamp()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    host.setRequesterBundled([](const QString&) { return true; });
    makeReady(host);
    m.work.append(pending("w1"));
    host.pullWork();
    QVERIFY(m.opened().isEmpty());
}

void TestIntentsModuleHost::aDoubleClickOpensOneRequest()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    host.setAppOfModule(&appOfBackend);
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    QTest::mouseDClick(&view, Qt::LeftButton);
    m.work.append(pending("w1"));
    m.work.append(pending("w2"));
    host.pullWork();
    QCOMPARE(m.opened(), QStringList{QStringLiteral("w1")});
}

// A click before the module restarted is no click for the new one's requests.
void TestIntentsModuleHost::aRestartForgetsUnspentClicks()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setAttention(QStringLiteral("interactive"));
    host.setAppOfModule(&appOfBackend);
    makeReady(host);
    QWidget view;
    host.watchView(QStringLiteral("wallet"), &view);
    QTest::mousePress(&view, Qt::LeftButton);
    m.restart();
    host.restarted();
    m.work.append(pending("w1"));
    host.pullWork();
    QVERIFY(m.opened().isEmpty());
}

QJsonObject deliverConfirm(const char* id)
{
    return item("deliver", id, {{QStringLiteral("intent"), QStringLiteral("basecamp.packages.confirm_install")},
                                {QStringLiteral("requester"), QStringLiteral("package_manager_ui_backend")}});
}

// The request behind a shell dialog ended: the dialog has to go.
void TestIntentsModuleHost::aDismissedShellDeliveryEndsItsDialog()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setShellProvider([](const QString&, const QString&, const QVariantMap&, const QString&) { return 1; });
    QSignalSpy ended(&host, &IntentsModuleHost::shellDeliveryEnded);
    makeReady(host);
    m.work.append(deliverConfirm("w1"));
    host.pullWork();
    QCOMPARE(ended.size(), 0);
    m.work.append(item("dismiss", "wd", {{QStringLiteral("work"), QStringLiteral("w1")}}));
    host.pullWork();
    QCOMPARE(ended.size(), 1);
    QCOMPARE(ended[0].value(0).toString(), QStringLiteral("w1"));
    QVERIFY(!host.ownsShellDelivery(QStringLiteral("w1")));
}

void TestIntentsModuleHost::aRestartEndsTheShellsDeliveries()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    host.setShellProvider([](const QString&, const QString&, const QVariantMap&, const QString&) { return 1; });
    QSignalSpy ended(&host, &IntentsModuleHost::shellDeliveryEnded);
    makeReady(host);
    m.work.append(deliverConfirm("w1"));
    host.pullWork();
    m.restart();
    host.restarted();
    QCOMPARE(ended.size(), 1);
    QCOMPARE(ended[0].value(0).toString(), QStringLiteral("w1"));
}

// Only a request that took the person somewhere brings them back.
void TestIntentsModuleHost::aRequestThatTookNobodyAnywhereBringsNobodyBack()
{
    FakeModule m;
    FakePresenter p;
    p.loaded = {QStringLiteral("wallet"), QStringLiteral("signer")};
    p.frontmost = QStringLiteral("signer");
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setReturnDwellFloorMs(0);
    makeReady(host);
    backendsUp(host);
    m.work.append(completed("d1"));
    host.pullWork();
    QVERIFY(p.presented.isEmpty());
}

// A return still waiting out its dwell when the module restarts is dropped.
void TestIntentsModuleHost::aDelayedReturnIsDroppedByARestart()
{
    FakeModule m;
    FakePresenter p;
    p.loaded = {QStringLiteral("wallet"), QStringLiteral("signer")};
    IntentsModuleHost host(m.call());
    host.setPresenter(&p);
    host.setReturnDwellFloorMs(100);
    makeReady(host);
    backendsUp(host);
    m.work.append(present("d1"));
    m.work.append(completed("d1"));
    host.pullWork();
    m.restart();
    host.restarted();
    QTest::qWait(250);
    QVERIFY(!p.presented.contains(QStringLiteral("wallet")));
}

// Someone else may hold the new instance: Basecamp says so, and stops
// loading it again.
void TestIntentsModuleHost::aModuleThatCantBeClaimedAgainIsGivenUp()
{
    FakeModule m;
    IntentsModuleHost host(m.call());
    QSignalSpy lost(&host, &IntentsModuleHost::presenterRoleLost);
    makeReady(host);
    QVERIFY(host.takeReload(false));
    m.restart();
    m.secret = QStringLiteral("someone-else");
    host.reloadFinished(true);
    QCOMPARE(lost.size(), 1);
    QVERIFY(!host.isClaimed());
    QVERIFY(!host.takeReload(false));
    QVERIFY(!host.shellRequest(QStringLiteral("packages.show"), {}, false));
}

QTEST_MAIN(TestIntentsModuleHost)
#include "intents_module_host_test.moc"
