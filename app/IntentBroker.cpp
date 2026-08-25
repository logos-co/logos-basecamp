#include "IntentBroker.h"

#include "IntentRegistry.h"
#include "LogosIntent.h"

#include <QDebug>
#include <QTimer>
#include <QUuid>

namespace {

constexpr int kSweepIntervalMs = 250;

// Not a decision deadline — a leak backstop for a chooser that was mounted and
// then vanished, long enough that a real person reading a dialog never hits it.
// Reported as `cancelled`: from the requester's side that is what it looks like.
constexpr int kChooserBackstopMs = 10 * 60 * 1000;

QString mintDispatchId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

IntentBroker::IntentBroker(IntentRegistry* registry,
                           IntentPresenter* presenter,
                           QObject* parent)
    : QObject(parent)
    , m_registry(registry)
    , m_presenter(presenter)
{
    m_clock.start();

    // ONE timer for every deadline. Per-request QTimers would be hundreds of
    // objects with individually wrong lifetimes; a timer in the bridge would be
    // a second clock that can disagree about the same request.
    m_sweepTimer = new QTimer(this);
    m_sweepTimer->setInterval(kSweepIntervalMs);
    connect(m_sweepTimer, &QTimer::timeout, this, &IntentBroker::sweep);
}

IntentBroker::~IntentBroker() = default;

qint64 IntentBroker::nowMs() const
{
    return m_clock.elapsed();
}

void IntentBroker::setTimeouts(int activationMs, int responseMs, int errorFloorMs)
{
    m_activationTimeoutMs = activationMs;
    m_responseTimeoutMs = responseMs;
    m_errorFloorMs = errorFloorMs;
}

int IntentBroker::pendingCount() const
{
    return m_pending.size();
}

// ── Endpoint bookkeeping ─────────────────────────────────────────────────────

void IntentBroker::setPresenter(IntentPresenter* presenter)
{
    m_presenter = presenter;
}

void IntentBroker::setChooser(IntentChooser* chooser)
{
    m_chooser = chooser;
}

void IntentBroker::setInstaller(IntentInstaller* installer)
{
    m_installer = installer;
}

bool IntentBroker::isShellProvider(const QString& providerName) const
{
    return m_registry && m_registry->isShellProvider(providerName);
}

void IntentBroker::registerEndpoint(const QString& appName, IntentEndpoint* endpoint)
{
    if (!endpoint || appName.isEmpty())
        return;

    // One bridge per app is an invariant the requester's identity rests on. If
    // a pointer ever appears under two names, two apps have silently collapsed
    // into one identity.
    const QString existing = m_endpointNames.value(endpoint);
    if (!existing.isEmpty() && existing != appName) {
        qWarning() << "IntentBroker: endpoint already registered as" << existing
                   << "— refusing to also register it as" << appName;
        return;
    }

    m_endpointNames.insert(endpoint, appName);
    m_endpointsByName.insert(appName, endpoint);
}

void IntentBroker::unregisterEndpoint(IntentEndpoint* endpoint)
{
    endpointDestroyed(endpoint);
}

QString IntentBroker::nameForEndpoint(IntentEndpoint* endpoint) const
{
    return m_endpointNames.value(endpoint);
}

// ── Submission ───────────────────────────────────────────────────────────────

void IntentBroker::submit(IntentEndpoint* from, const QString& requestId,
                          const QString& intent, const QVariantMap& params)
{
    if (!from)
        return;

    // Type bound, before anything is recorded: can this value cross between two
    // QML engines at all — depth, size, and a closed type list refusing QObject*
    // and QJSValue. Intent-independent, so it needs no registry lookup.
    //
    // Floored rather than answered inline, because a provider may also mint
    // bad_request: an instant answer would tell the caller no provider was
    // consulted, the existence oracle the merged `unavailable` prevents.
    const bool payloadOk = logos::intent::isCanonicalPayload(params);
    if (!payloadOk) {
        qWarning().noquote()
            << "IntentBroker: refusing non-canonical params for" << intent
            << "— a payload must be plain data (see isCanonicalPayload)";
    }

    PendingRequest request;
    request.dispatchId = mintDispatchId();
    request.requestId = requestId;
    request.requester = from;
    request.requesterName = nameForEndpoint(from);
    request.intent = intent;
    request.params = params;
    request.phase = Phase::Accepted;
    request.acceptedAtMs = nowMs();
    request.phaseSinceMs = request.acceptedAtMs;


    m_pending.insert(request.dispatchId, request);

    if (!m_sweepTimer->isActive())
        m_sweepTimer->start();

    // QUEUED, ALWAYS. Every entry point is reachable from a QML click handler,
    // and working on that stack is the known crash shape here: a delegate's
    // handler still on the stack while the work tears down the item owning it.
    const QString dispatchId = request.dispatchId;
    if (!payloadOk) {
        failWithFloor(dispatchId, logos::intent::errBadRequest());
        return;
    }

    QMetaObject::invokeMethod(this, [this, dispatchId]() {
        startRequest(dispatchId);
    }, Qt::QueuedConnection);
}

void IntentBroker::startRequest(const QString& dispatchId)
{
    auto it = m_pending.find(dispatchId);
    if (it == m_pending.end())
        return;   // requester died before we got here

    const QString intent = it->intent;
    const QString requesterName = it->requesterName;

    if (!m_registry) {
        failWithFloor(dispatchId, logos::intent::errUnavailable());
        return;
    }

    // R7: declaration is mandatory. This is the one error that is NOT floored —
    // it is a fact about the caller's own manifest and leaks nothing about what
    // else is installed.
    if (requesterName.isEmpty() || !m_registry->declaresUse(requesterName, intent)) {
        finish(dispatchId, false, QVariant(), logos::intent::errNotDeclared());
        return;
    }

    const IntentRegistry::Resolution resolution = m_registry->resolve(intent);

    if (resolution.status == IntentRegistry::None) {
        // Nothing installed services this, but the catalog might know a package
        // that would — the SHELL offers it. The requester learns nothing either
        // way: an app that could tell "we offered an install" from "no match
        // existed" could enumerate what is not installed.
        const QStringList installable =
            m_registry->installableProvidersFor(intent);

        if (installable.isEmpty()) {
            qWarning().noquote()
                << "IntentBroker: nothing installed provides" << intent
                << "and the catalog knows no package that would";
        }

        if (!installable.isEmpty() && m_installer) {
            qWarning().noquote()
                << "IntentBroker: suggesting an install for" << intent << "—"
                << installable.join(QStringLiteral(", "));

            // A SUGGESTION, not a continuation: the request is answered below
            // either way. If the user installs something they retry, and that
            // retry resolves like any other request.
            m_installer->offerInstall(intent, installable);
        }

        // Merged with "denied" deliberately. Answering here rather than after the
        // user decides also keeps the request's lifetime independent of how long
        // they deliberate, which would be a slower signal saying the same thing.
        failWithFloor(dispatchId, logos::intent::errUnavailable());
        return;
    }

    // AN APP ASKING FOR ITS OWN CAPABILITY dispatches straight to itself.
    // Declaring both `provides` and `uses` for one intent is reasonable — it is
    // internal navigation — and a chooser here would ask the user to pick an app
    // to answer itself: consent for a boundary that is not being crossed. The
    // name is HOST-ATTESTED, never taken from the payload.
    if (!requesterName.isEmpty() && m_registry->declaresProvide(requesterName, intent)) {
        chooseProvider(dispatchId, requesterName);
        return;
    }

    if (resolution.status == IntentRegistry::Ok) {
        // ONE PROVIDER IS NOT A REASON TO SKIP THE USER — dispatching straight
        // through would make the single-provider case the silent one, and the
        // ambiguous case the safer, which is backwards. Both exemptions are the
        // shell: it cannot be impersonated (the registry refuses `logos.*` from
        // disk records), and confirming the navigation the user just clicked is a
        // dialog answering itself. Requester-side, only with exactly one
        // provider: if several could answer, WHICH one still deserves a choice.
        const QString only = resolution.found.first().moduleName;
        if (isShellProvider(only) || isShellProvider(requesterName)) {
            chooseProvider(dispatchId, only);
            return;
        }
        // Fall through: confirm it like any other choice, with one candidate.
    }

    // AwaitingChoice has NO deadline in sweep(): a human is deciding and killing
    // that mid-thought is worse than waiting. That only holds if a chooser really
    // exists, so present() reports its receiver count and we fail closed on zero.
    // Asking the seam rather than this object's own signal is the point — the
    // shell always connects it to re-emit for QML, so the count here would be 1
    // with no dialog mounted and the request would hang for the process lifetime.
    QVariantList providers;
    for (const IntentRegistry::ProviderEntry& entry : resolution.found) {
        providers.append(QVariantMap{
            { QStringLiteral("moduleName"), entry.moduleName },
            { QStringLiteral("displayName"), entry.displayName },
            { QStringLiteral("iconSource"), entry.iconSource },
        });
    }

    it->phase = Phase::AwaitingChoice;
    it->phaseSinceMs = nowMs();
    it->choiceProviders = providers;

    // QUEUE, DO NOT REPOINT. openWith() on a visible chooser silently swaps its
    // contents, so a user reaching for a provider row would answer a request
    // that arrived a moment ago instead of the one they read — and an app
    // controls its own request timing, making that a consent swap.
    //
    // Queued broker-side, not refused dialog-side: a QML refusal returns a
    // receiver count of 0, which reads as "no chooser exists" and becomes
    // `unavailable`. Arriving at a busy moment must not fail a request.
    if (aChoiceIsOnScreen())
        return;   // drainChoiceQueue() picks it up when the current one ends

    presentPendingChoice(dispatchId);
}

bool IntentBroker::aChoiceIsOnScreen() const
{
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it->choicePresented && it->phase == Phase::AwaitingChoice)
            return true;
    }
    return false;
}

void IntentBroker::presentPendingChoice(const QString& dispatchId)
{
    auto it = m_pending.find(dispatchId);
    if (it == m_pending.end() || it->choicePresented)
        return;

    const int shown = m_chooser
        ? m_chooser->present(dispatchId, it->intent, it->requesterName,
                             it->choiceProviders)
        : 0;
    if (shown == 0) {
        qWarning() << "IntentBroker: no chooser mounted for" << it->intent
                   << "— answering unavailable";
        failWithFloor(dispatchId, logos::intent::errUnavailable());
        return;
    }
    it->choicePresented = true;
    emit chooserRequested(dispatchId, it->intent, it->requesterName,
                          it->choiceProviders);
}

void IntentBroker::drainChoiceQueue()
{
    if (aChoiceIsOnScreen())
        return;

    // Oldest first. A non-deterministic order would make which request the user
    // is answering depend on hash iteration order.
    QString oldest;
    qint64 oldestAt = 0;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it->choicePresented) continue;
        if (it->phase != Phase::AwaitingChoice) continue;
        if (oldest.isEmpty() || it->acceptedAtMs < oldestAt) {
            oldest = it.key();
            oldestAt = it->acceptedAtMs;
        }
    }
    if (!oldest.isEmpty())
        presentPendingChoice(oldest);
}

void IntentBroker::resolveChooser(const QString& dispatchId,
                                  const QString& providerName)
{
    auto it = m_pending.find(dispatchId);
    if (it == m_pending.end() || it->phase != Phase::AwaitingChoice)
        return;

    if (!m_registry || !m_registry->declaresProvide(providerName, it->intent)) {
        failWithFloor(dispatchId, logos::intent::errUnavailable());
        return;
    }

    chooseProvider(dispatchId, providerName);
}

void IntentBroker::cancelChooser(const QString& dispatchId)
{
    auto it = m_pending.find(dispatchId);
    if (it == m_pending.end() || it->phase != Phase::AwaitingChoice)
        return;

    // Indistinguishable from a provider-side cancel, deliberately.
    finish(dispatchId, false, QVariant(), logos::intent::errCancelled());
}

void IntentBroker::chooseProvider(const QString& dispatchId, const QString& providerName)
{
    auto it = m_pending.find(dispatchId);
    if (it == m_pending.end())
        return;

    it->providerName = providerName;
    if (it->choicePresented) {
        if (m_chooser) m_chooser->dismiss(dispatchId);
        emit chooserDismissed(dispatchId);
        it->choicePresented = false;
    }

    // The dialog is free the moment a choice is MADE, not when the request
    // finally completes — a provider can take as long as it likes to answer,
    // and queueing the next question behind that would look like the shell
    // had ignored it. Deferred so the current request finishes its own
    // dispatch first; presenting mid-dispatch would re-enter this file.
    it->choicePresented = false;
    QMetaObject::invokeMethod(this, [this]() { drainChoiceQueue(); },
                              Qt::QueuedConnection);

    // The host is always up. Skip activation entirely — there is nothing to
    // load, and routing it through the presenter makes basecamp try to load
    // itself as a plugin.
    if (isShellProvider(providerName)) {
        dispatchTo(dispatchId);
        return;
    }

    if (!m_presenter) {
        failWithFloor(dispatchId, logos::intent::errUnavailable());
        return;
    }

    if (m_presenter->isAppLoaded(providerName)) {
        dispatchTo(dispatchId);
        return;
    }

    it->phase = Phase::Activating;
    it->phaseSinceMs = nowMs();

    // Ask the shell to load it exactly once, however many requests are queued.
    QStringList& queue = m_activationQueue[providerName];
    const bool alreadyRequested = !queue.isEmpty();
    queue.append(dispatchId);
    if (!alreadyRequested)
        m_presenter->ensureAppLoaded(providerName);
}

void IntentBroker::onAppReady(const QString& appName)
{
    drainActivationQueue(appName, /*ready=*/true);
}

void IntentBroker::onAppUnavailable(const QString& appName)
{
    drainActivationQueue(appName, /*ready=*/false);
}

void IntentBroker::drainActivationQueue(const QString& appName, bool ready)
{
    const QStringList queued = m_activationQueue.take(appName);
    for (const QString& dispatchId : queued) {
        if (!m_pending.contains(dispatchId))
            continue;
        if (ready)
            dispatchTo(dispatchId);
        else
            failWithFloor(dispatchId, logos::intent::errUnavailable());
    }
}

// Does a value look like the type the provider said it wanted? Deliberately
// lenient about number width — QML hands us Int or Double depending on whether
// the literal had a fractional part, and no provider means to distinguish those.
static bool matchesDeclaredType(const QVariant& v, const QString& declared)
{
    const int t = v.userType();
    if (declared == QLatin1String("string"))
        return t == QMetaType::QString;
    if (declared == QLatin1String("number"))
        return t == QMetaType::Int    || t == QMetaType::UInt
            || t == QMetaType::LongLong || t == QMetaType::ULongLong
            || t == QMetaType::Double || t == QMetaType::Float;
    if (declared == QLatin1String("bool"))
        return t == QMetaType::Bool;
    if (declared == QLatin1String("object"))
        return t == QMetaType::QVariantMap;
    if (declared == QLatin1String("array"))
        return t == QMetaType::QVariantList || t == QMetaType::QStringList;

    // A type we do not recognise. The provider may be ahead of us, and refusing
    // a payload because the SHELL is out of date would be the wrong party
    // paying for it.
    return true;
}

QString IntentBroker::specViolation(const QString& providerName,
                                    const QString& intent,
                                    const QVariantMap& params) const
{
    if (!m_registry)
        return QString();

    const QVariantList spec = m_registry->paramsSpecFor(providerName, intent);
    if (spec.isEmpty())
        return QString();   // undescribed — not the same as "takes nothing"

    for (const QVariant& entry : spec) {
        const QVariantMap p = entry.toMap();
        const QString name = p.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;

        const bool present = params.contains(name);
        if (!present) {
            if (p.value(QStringLiteral("required")).toBool())
                return QStringLiteral("missing required parameter '%1'").arg(name);
            continue;
        }

        const QString type = p.value(QStringLiteral("type")).toString();
        if (!type.isEmpty() && !matchesDeclaredType(params.value(name), type)) {
            return QStringLiteral("parameter '%1' should be %2").arg(name, type);
        }
    }

    // Params the provider did not describe are allowed through. A caller
    // written against a newer version of a provider must not be broken by the
    // older description, and a provider is free to accept more than it lists.
    return QString();
}

void IntentBroker::dispatchTo(const QString& dispatchId)
{
    auto it = m_pending.find(dispatchId);
    if (it == m_pending.end())
        return;

    IntentEndpoint* provider = m_endpointsByName.value(it->providerName);
    if (!provider) {
        failWithFloor(dispatchId, logos::intent::errUnavailable());
        return;
    }

    // Last gate before another app is handed the payload. The reason goes to
    // the log, not to the requester: the result surface carries a code and
    // nothing else, so `bad_request` is what the app sees and this line is
    // what its developer reads.
    const QString violation = specViolation(it->providerName, it->intent, it->params);
    if (!violation.isEmpty()) {
        qWarning() << "IntentBroker: refusing" << it->intent << "from"
                   << it->requesterName << "—" << violation
                   << "(declared by" << it->providerName << "in its metadata.json)";
        failWithFloor(dispatchId, logos::intent::errBadRequest());
        return;
    }

    // POINTER identity, recorded now and compared on response. A reloaded app
    // is a different endpoint and must not inherit the old one's requests.
    it->provider = provider;
    it->phase = Phase::Dispatched;
    it->phaseSinceMs = nowMs();

    // Not for the shell: it has no widget to raise, and its handler does its
    // own navigating (the repositories intent lands the user on Settings).
    // Calling presentApp here would either no-op or fight that.
    if (m_presenter && !isShellProvider(it->providerName))
        m_presenter->presentApp(it->providerName);

    const int receivers = provider->deliverRequest(dispatchId, it->intent,
                                                   it->params, it->requesterName);
    it->providerHadHandler = (receivers > 0);
    if (receivers == 0) {
        // Declared the capability, shipped no handler. Left to time out rather
        // than failed immediately: a handler installed from an async Loader or
        // a later Component.onCompleted legitimately arrives after this point,
        // and the deadline is the honest bound on "it never showed up".
        qWarning() << "IntentBroker: provider" << it->providerName
                   << "has no handler for" << it->intent
                   << "— the request will time out";
    }
}

// ── Completion ───────────────────────────────────────────────────────────────

void IntentBroker::submitResponse(IntentEndpoint* from, const QString& dispatchId,
                                  bool ok, const QVariant& data, const QString& error)
{
    auto it = m_pending.find(dispatchId);

    // Three checks, and every failure tells the responder nothing. One log line
    // for all of them: the distinction matters to us, never to the caller.
    const char* reject = nullptr;
    if (it == m_pending.end())            reject = "unknown dispatch id";
    else if (it->phase != Phase::Dispatched) reject = "not dispatched";
    else if (it->provider != from)        reject = "wrong endpoint";
    if (reject) {
        qWarning() << "IntentBroker: dropped response —" << reject;
        return;
    }

    // A provider's response payload crosses back into the requester's engine, so
    // it gets the same type bound the request did. This one stays `failed`, not
    // bad_request: the REQUESTER did nothing wrong, and telling it otherwise
    // would send a developer hunting through their own call site.
    if (!logos::intent::isCanonicalValue(data)) {
        qWarning().noquote()
            << "IntentBroker: provider" << it->providerName
            << "answered" << it->intent << "with a non-canonical payload — dropped";
        finish(dispatchId, false, QVariant(), logos::intent::errFailed());
        return;
    }

    // A provider may only report cancelled / timeout / failed / bad_request.
    // Free text and the two broker-only codes are coerced here, at the boundary.
    const QString normalized = logos::intent::normalizeError(ok, error);
    finish(dispatchId, ok, data, normalized);
}

void IntentBroker::failWithFloor(const QString& dispatchId, const QString& errorCode)
{
    auto it = m_pending.find(dispatchId);
    if (it == m_pending.end())
        return;

    const qint64 elapsed = nowMs() - it->acceptedAtMs;
    const qint64 remaining = static_cast<qint64>(m_errorFloorMs) - elapsed;
    if (remaining <= 0) {
        finish(dispatchId, false, QVariant(), errorCode);
        return;
    }

    // Hold it. A caller that gets "no" instantly has learned that nothing is
    // installed without ever being allowed to ask what is.
    QTimer::singleShot(static_cast<int>(remaining), this, [this, dispatchId, errorCode]() {
        if (m_pending.contains(dispatchId))
            finish(dispatchId, false, QVariant(), errorCode);
    });
}

void IntentBroker::finish(const QString& dispatchId, bool ok,
                          const QVariant& data, const QString& error)
{
    auto it = m_pending.find(dispatchId);
    if (it == m_pending.end())
        return;

    const PendingRequest request = it.value();
    m_pending.erase(it);   // erase before delivering: fire-once by construction

    // dismiss() used to be emitted only from chooseProvider(), so the dialog
    // came down when a choice was MADE and never when a request ended any other
    // way — a backstop firing, a requester dying, an abandon. The record went,
    // the dialog stayed.
    if (request.choicePresented) {
        if (m_chooser) m_chooser->dismiss(dispatchId);
        emit chooserDismissed(dispatchId);
    }

    // Drop this request from any activation queue it was still sitting in.
    if (!request.providerName.isEmpty()) {
        auto queueIt = m_activationQueue.find(request.providerName);
        if (queueIt != m_activationQueue.end()) {
            queueIt->removeAll(dispatchId);
            if (queueIt->isEmpty())
                m_activationQueue.erase(queueIt);
        }
    }

    deliver(request, logos::intent::makeEnvelope(ok, data, error));

    // The dialog may have just come free. Anything queued behind it gets its
    // turn now rather than waiting out the backstop.
    drainChoiceQueue();

    if (m_pending.isEmpty())
        m_sweepTimer->stop();
}

void IntentBroker::deliver(const PendingRequest& request, const QVariantMap& envelope)
{
    if (!request.requester)
        return;
    // R5: the result goes to the requester alone, under the requester's OWN id.
    request.requester->deliverResult(request.requestId, envelope);
}


// ── Lifetime and deadlines ───────────────────────────────────────────────────

void IntentBroker::abandon(IntentEndpoint* from, const QStringList& requestIds)
{
    // The bridge dropped these callbacks (a hot reload rebound its engine), so
    // there is nobody left to answer. Clear our side too rather than leave
    // records that can never complete.
    const QStringList ids = m_pending.keys();
    for (const QString& dispatchId : ids) {
        auto it = m_pending.find(dispatchId);
        if (it == m_pending.end())
            continue;
        if (it->requester != from || !requestIds.contains(it->requestId))
            continue;

        it->requester = nullptr;
        finish(dispatchId, false, QVariant(), logos::intent::errCancelled());
    }
    if (m_pending.isEmpty())
        m_sweepTimer->stop();
}

void IntentBroker::endpointDestroyed(IntentEndpoint* endpoint)
{
    if (!endpoint)
        return;

    const QString name = m_endpointNames.take(endpoint);
    if (!name.isEmpty() && m_endpointsByName.value(name) == endpoint)
        m_endpointsByName.remove(name);

    const QStringList ids = m_pending.keys();
    for (const QString& dispatchId : ids) {
        auto it = m_pending.find(dispatchId);
        if (it == m_pending.end())
            continue;

        if (it->requester == endpoint) {
            // Nobody to deliver to. There is no cancel symbol in the frozen
            // surface, so a later provider response is simply dropped as an
            // unknown id.
            it->requester = nullptr;
            finish(dispatchId, false, QVariant(), logos::intent::errCancelled());
            continue;
        }

        if (it->provider == endpoint) {
            // "unavailable", not "failed": distinguishing "was reached and
            // died" from "was never there" leaks install state.
            failWithFloor(dispatchId, logos::intent::errUnavailable());
        }
    }

    if (m_pending.isEmpty())
        m_sweepTimer->stop();
}

void IntentBroker::sweep()
{
    const qint64 now = nowMs();
    const QStringList ids = m_pending.keys();

    for (const QString& dispatchId : ids) {
        auto it = m_pending.find(dispatchId);
        if (it == m_pending.end())
            continue;

        const qint64 inPhase = now - it->phaseSinceMs;

        switch (it->phase) {
        case Phase::Activating:
            // Sized for the real worst case: the view-host ready deadline alone
            // is 30 s before any QML compile, and some apps add their own
            // readiness gate on top.
            if (inPhase > m_activationTimeoutMs)
                failWithFloor(dispatchId, logos::intent::errUnavailable());
            break;

        case Phase::Dispatched:
            if (inPhase > (it->providerHadHandler ? kChooserBackstopMs
                                                  : m_responseTimeoutMs))
                finish(dispatchId, false, QVariant(), logos::intent::errTimeout());
            break;

        case Phase::AwaitingChoice:
            // Effectively no deadline — a human is looking at a dialog and
            // killing that mid-thought is worse than waiting. The backstop is
            // for the case the guard above cannot see: a chooser that WAS
            // mounted and then went away (view torn down, shell reloaded),
            // leaving a request nobody will ever answer.
            if (inPhase > kChooserBackstopMs)
                failWithFloor(dispatchId, logos::intent::errCancelled());
            break;

        case Phase::Accepted:
            // Queued dispatch runs on the next event-loop turn; if a request is
            // still here after the activation budget, something ate the post.
            if (inPhase > m_activationTimeoutMs)
                failWithFloor(dispatchId, logos::intent::errUnavailable());
            break;
        }
    }

    if (m_pending.isEmpty())
        m_sweepTimer->stop();
}
