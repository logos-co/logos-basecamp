#include "IntentsModuleHost.h"

#include "utils/ManifestFile.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QEvent>
#include <QJsonDocument>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace {

// The auto-return breaker, as IntentBroker's.
constexpr qint64 kReturnBreakerWindowMs = 3 * 1000;
constexpr int    kReturnBreakerLimit    = 5;

constexpr int kMaxReloads = 3;

bool isInput(QEvent::Type type)
{
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::TouchBegin:
    case QEvent::KeyPress:
        return true;
    default:
        return false;
    }
}

QVariantMap failedEnvelope()
{
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("failed")}};
}

QString compact(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString compact(const QJsonArray& a)
{
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

// The SHA-256 of `<dir>/metadata.json` (logos::manifest::read()): "" when
// there is none, "unreadable" when it can't be read that way.
QString manifestRevision(const QString& dir)
{
    QByteArray bytes;
    switch (logos::manifest::read(dir, &bytes)) {
    case logos::manifest::Read::Ok:
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    case logos::manifest::Read::Missing:
        return {};
    default:
        return QStringLiteral("unreadable");
    }
}

// What the module reads from each package, as it is now: an app upgraded in
// place keeps its entry but not its manifest.
QStringList manifestRevisions(const QJsonArray& packages)
{
    QStringList out;
    for (const QJsonValue& p : packages)
        out << manifestRevision(p.toObject().value(QStringLiteral("installDir")).toString());
    return out;
}

} // namespace

IntentsModuleHost::IntentsModuleHost(Call call, QObject* parent)
    : QObject(parent)
    , m_call(std::move(call))
    , m_syncTimer(new QTimer(this))
{
    m_clock.start();
    m_syncTimer->setSingleShot(true);
    connect(m_syncTimer, &QTimer::timeout, this, &IntentsModuleHost::sync);
}

void IntentsModuleHost::callJson(const QString& method, const QVariantList& args,
                                 std::function<void(const QJsonObject&)> then, std::function<void()> dropped)
{
    const quint64 call = ++m_nextCall;
    m_inFlight.insert(call, std::move(dropped));
    QPointer<IntentsModuleHost> self(this);
    Reply reply = [self, call, then = std::move(then)](const QString& text) {
        // Already answered by `dropped`, when the instance went first.
        if (!self || !self->m_inFlight.remove(call)) return;
        if (then) then(QJsonDocument::fromJson(text.toUtf8()).object());
    };
    if (m_call) m_call(method, args, std::move(reply));
    else reply(QString());
}

void IntentsModuleHost::claim(std::function<void(bool)> done)
{
    if (isClaimed()) {
        if (done) done(true);
        return;
    }
    callJson(QStringLiteral("claim"), {}, [this, done](const QJsonObject& r) {
        m_secret = r.value(QStringLiteral("ok")).toBool() ? r.value(QStringLiteral("secret")).toString()
                                                          : QString();
        if (m_secret.isEmpty()) qWarning() << "intents_module: could not claim the presenter role";
        if (done) done(isClaimed());
    });
}

void IntentsModuleHost::restarted()
{
    if (!isClaimed()) {
        reclaim();
        return;
    }
    // Still the instance this secret was minted by: nothing was lost.
    callJson(QStringLiteral("next_work"), {m_secret}, [this](const QJsonObject& r) {
        if (r.value(QStringLiteral("ok")).toBool()) take(r.value(QStringLiteral("items")).toArray());
        else reclaim();
    });
}

void IntentsModuleHost::reclaim()
{
    forgetInstance();
    claim([this](bool claimed) {
        if (!claimed) {
            m_gaveUp = true;
            emit presenterRoleLost();
            return;
        }
        // Said, and its work pulled, once it knows what the old instance did.
        m_reclaimed = true;
        m_workWaiting = true;
        sync();
    });
}

// Everything the instance had open went with it, the claim too, and what was
// on its way to it is heard by nobody.
void IntentsModuleHost::forgetInstance()
{
    ++m_generation;
    for (const std::function<void()>& dropped : std::exchange(m_inFlight, {}))
        if (dropped) dropped();
    m_syncing = false;
    m_pulling = false;
    m_pullAgain = false;
    const QHash<QString, ResultFn> shellRequests = std::exchange(m_shellRequests, {});
    for (const ResultFn& done : shellRequests) done(failedEnvelope());
    failWaitingRequests();
    const QSet<QString> choices = m_choices;
    m_choices.clear();
    m_choiceModules.clear();
    for (const QString& id : choices) emit chooserDismissed(id);
    for (const QString& id : std::exchange(m_shellDeliveries, {})) emit shellDeliveryEnded(id);
    m_starting.clear();
    for (QTimer* timer : std::exchange(m_startTimers, {})) timer->deleteLater();
    m_navigatedAt.clear();
    m_inputAtMs.clear();
    m_heldWork = {};
    m_workWaiting = false;
    m_reclaimed = false;
    m_syncTimer->stop();
    m_syncDelayMs = m_syncRetryMs;
    m_shellAcked = false;
    m_installedAcked = false;
    m_secret.clear();
}

bool IntentsModuleHost::isReady() const
{
    return isClaimed() && m_shellDeclared && m_shellAcked && m_installedSent && m_installedAcked;
}

// What the module has to hold before Basecamp asks or answers through it, one
// call at a time. An attempt ends at its first failed call (its 3 s timeout,
// when the module doesn't answer), and each failed attempt waits twice as long
// for the next, up to a minute; work the module hands out meanwhile brings
// the next attempt forward.
void IntentsModuleHost::sync()
{
    if (!isClaimed() || m_syncing) return;
    if (m_shellDeclared && !m_shellAcked) {
        syncStep(QStringLiteral("declare_shell"), compact(m_shell), m_shellVersion, &m_shellVersion, &m_shellAcked);
        return;
    }
    if (m_installedSent && !m_installedAcked) {
        syncStep(QStringLiteral("set_installed"), compact(m_installed), m_installedVersion, &m_installedVersion,
                 &m_installedAcked);
        return;
    }
    m_syncDelayMs = m_syncRetryMs;
    if (!isReady()) {
        failWaitingRequests();
        return;
    }
    if (std::exchange(m_reclaimed, false)) qInfo() << "intents_module: claimed again after it restarted";
    take({});
    for (ShellAsk& ask : std::exchange(m_asksWaiting, {}))
        sendShellRequest(ask.intent, ask.params, ask.asLink, std::move(ask.done));
    if (m_workWaiting) pullWork();
}

// Taken only while still the latest: a newer declaration or list, set while
// this one was on its way, goes next.
void IntentsModuleHost::syncStep(const QString& method, const QString& payload, quint64 version,
                                 const quint64* current, bool* acked)
{
    m_syncing = true;
    callJson(method, {m_secret, payload}, [this, method, version, current, acked](const QJsonObject& r) {
        m_syncing = false;
        if (!r.value(QStringLiteral("ok")).toBool()) {
            qWarning().noquote() << "intents_module:" << method << "not taken; sending it again in"
                                 << m_syncDelayMs << "ms";
            m_syncTimer->start(m_syncDelayMs);
            m_syncDelayMs = std::min(m_syncDelayMs * 2, m_syncRetryCapMs);
            failWaitingRequests();
            return;
        }
        if (*current == version) *acked = true;
        sync();
    });
}

bool IntentsModuleHost::takeReload(bool moduleRunning)
{
    if (moduleRunning || m_reloading || m_shuttingDown || m_gaveUp) return false;
    if (m_reloads >= kMaxReloads) {
        // Off for the session: nothing is asked of a module that isn't there.
        if (isClaimed()) qWarning() << "intents_module stopped again; it is off for this session";
        forgetInstance();
        return false;
    }
    m_reloading = true;
    ++m_reloads;
    return true;
}

void IntentsModuleHost::reloadFinished(bool loaded)
{
    m_reloading = false;
    if (m_shuttingDown) return;
    if (loaded) {
        restarted();
        return;
    }
    qWarning().noquote() << "intents_module did not load again;"
                         << (m_reloads < kMaxReloads ? "it is tried again when the modules next change"
                                                     : "it is off for this session");
    forgetInstance();
}

// A retry already due sends it: a module that doesn't answer isn't asked sooner.
void IntentsModuleHost::declareShell(const QJsonObject& shell)
{
    m_shell = shell;
    m_shellDeclared = true;
    m_shellAcked = false;
    ++m_shellVersion;
    if (!m_syncTimer->isActive()) sync();
}

void IntentsModuleHost::setInstalled(const QJsonArray& packages)
{
    const QStringList revisions = manifestRevisions(packages);
    if (m_installedSent && packages == m_installed && revisions == m_installedRevisions) return;
    m_installed = packages;
    m_installedRevisions = revisions;
    m_installedSent = true;
    m_installedAcked = false;
    ++m_installedVersion;
    if (!m_syncTimer->isActive()) sync();
}

void IntentsModuleHost::shellRequest(const QString& intent, const QVariantMap& params, bool asLink,
                                     ResultFn done)
{
    if (isReady()) {
        sendShellRequest(intent, params, asLink, std::move(done));
        return;
    }
    // Being told what it needs right now, as when a link that launched
    // Basecamp goes right after the installed list: it waits for that.
    if (isClaimed() && m_syncing) {
        m_asksWaiting.append({intent, params, asLink, std::move(done)});
        return;
    }
    if (done) done(failedEnvelope());
}

void IntentsModuleHost::failWaitingRequests()
{
    for (const ShellAsk& ask : std::exchange(m_asksWaiting, {}))
        if (ask.done) ask.done(failedEnvelope());
}

void IntentsModuleHost::sendShellRequest(const QString& intent, const QVariantMap& params, bool asLink,
                                         ResultFn done)
{
    const auto failed = [done]() {
        if (done) done(failedEnvelope());
    };
    callJson(QStringLiteral("shell_request"),
             {m_secret, intent, compact(QJsonObject::fromVariantMap(params)), asLink},
             [this, intent, asLink, done, failed](const QJsonObject& r) {
                 const QString handle = r.value(QStringLiteral("handle")).toString();
                 if (!r.value(QStringLiteral("ok")).toBool() || handle.isEmpty()) {
                     qWarning().noquote() << "intents_module refused" << intent << "from"
                                          << (asLink ? "a link" : "Basecamp") << ":"
                                          << r.value(QStringLiteral("error")).toString();
                     failed();
                     return;
                 }
                 if (done) m_shellRequests.insert(handle, done);
             },
             failed);
}

void IntentsModuleHost::registerBackend(const QString& app, const QString& module)
{
    m_backends.insert(app, module);
    finishStart(app);
}

void IntentsModuleHost::unregisterBackend(const QString& app)
{
    m_backends.remove(app);
    m_inputAtMs.remove(app);
}

void IntentsModuleHost::appReady(const QString& app)
{
    if (!m_backends.contains(app)) finishStart(app);
}

QString IntentsModuleHost::appOf(const QString& module) const
{
    for (auto it = m_backends.cbegin(); it != m_backends.cend(); ++it)
        if (it.value() == module) return it.key();
    return m_appOfModule ? m_appOfModule(module) : QString();
}

QString IntentsModuleHost::nameFor(const QString& module) const
{
    const QString app = appOf(module);
    return app.isEmpty() ? module : app;
}

// ── Clicks ───────────────────────────────────────────────────────────────────

void IntentsModuleHost::watchView(const QString& app, QWidget* view)
{
    if (!view) return;
    m_appOfView.insert(view, app);
    view->installEventFilter(this);
    connect(view, &QObject::destroyed, this, [this](QObject* gone) { m_appOfView.remove(gone); });
}

bool IntentsModuleHost::eventFilter(QObject* watched, QEvent* event)
{
    if (isInput(event->type()) && (event->spontaneous() || m_acceptSynthesized)) {
        const auto it = m_appOfView.constFind(watched);
        if (it != m_appOfView.constEnd()) m_inputAtMs.insert(it.value(), m_clock.elapsed());
    }
    return QObject::eventFilter(watched, event);
}

// One click opens at most one request, and only soon after it.
bool IntentsModuleHost::takeInput(const QString& app)
{
    if (app.isEmpty() || !m_inputAtMs.contains(app)) return false;
    return m_clock.elapsed() - m_inputAtMs.take(app) <= m_inputWindowMs;
}

// ── Work ─────────────────────────────────────────────────────────────────────

void IntentsModuleHost::pullWork()
{
    if (!isReady()) {
        m_workWaiting = isClaimed();
        // It hands out work, so it answers again: what it didn't take goes now,
        // not at the backoff's next turn, or a click behind that work expires.
        if (m_workWaiting && m_syncTimer->isActive()) {
            m_syncTimer->stop();
            sync();
        }
        return;
    }
    // One pull at a time: one asked for meanwhile goes once it is back.
    if (m_pulling) {
        m_pullAgain = true;
        return;
    }
    m_pulling = true;
    m_workWaiting = false;
    callJson(QStringLiteral("next_work"), {m_secret}, [this](const QJsonObject& r) {
        m_pulling = false;
        take(r.value(QStringLiteral("items")).toArray());
        if (std::exchange(m_pullAgain, false)) pullWork();
    });
}

// Work pulled before the module was ready is handled once it is.
void IntentsModuleHost::take(const QJsonArray& items)
{
    for (const QJsonValue& item : items) m_heldWork.append(item);
    if (!isReady()) return;
    for (const QJsonValue& item : std::exchange(m_heldWork, {})) handle(item.toObject());
}

void IntentsModuleHost::answer(const QString& workId, const QJsonObject& answer, std::function<void(bool)> taken)
{
    callJson(QStringLiteral("answer"), {m_secret, workId, compact(answer)},
             [workId, taken](const QJsonObject& r) {
                 const bool ok = r.value(QStringLiteral("ok")).toBool();
                 if (!ok) qWarning().noquote() << "intents_module: answer to" << workId << "not taken";
                 if (taken) taken(ok);
             },
             [taken]() {
                 if (taken) taken(false);
             });
}

void IntentsModuleHost::handle(const QJsonObject& item)
{
    const QString kind = item.value(QStringLiteral("kind")).toString();
    const QString id = item.value(QStringLiteral("id")).toString();

    if (kind == QStringLiteral("choose")) {
        // Basecamp's own request with one app to answer it, or with its
        // default app among them: the click in Basecamp was the choice, as on
        // the QML path. Only Basecamp can ask under its name; a link asks
        // under its own, so it is always shown.
        const QJsonArray offered = item.value(QStringLiteral("providers")).toArray();
        const QString shell = m_shell.value(QStringLiteral("name")).toString();
        QString picked;
        if (!shell.isEmpty() && item.value(QStringLiteral("requester")).toString() == shell) {
            const QString preferred = m_defaultProvider
                ? m_defaultProvider(item.value(QStringLiteral("intent")).toString()) : QString();
            for (const QJsonValue& p : offered) {
                const QString offeredModule = p.toObject().value(QStringLiteral("module")).toString();
                if (offered.size() == 1 || (!preferred.isEmpty() && offeredModule == preferred))
                    picked = offeredModule;
            }
        }
        if (!picked.isEmpty()) {
            const QJsonObject chosen{{QStringLiteral("action"), QStringLiteral("choose")},
                                     {QStringLiteral("provider"), picked}};
            QPointer<IntentsModuleHost> self(this);
            start(picked, [self, id, chosen]() {
                if (self) self->answer(id, chosen);
            });
            return;
        }
        // An app's backend is shown, and picked, as its app.
        QVariantList providers;
        QHash<QString, QString> modules;
        for (const QJsonValue& p : offered) {
            const QJsonObject o = p.toObject();
            const QString module = o.value(QStringLiteral("module")).toString();
            const QString shown = nameFor(module);
            modules.insert(shown, module);
            providers.append(QVariantMap{
                {QStringLiteral("moduleName"), shown},
                {QStringLiteral("displayName"), o.value(QStringLiteral("displayName")).toString()},
                {QStringLiteral("iconSource"), o.value(QStringLiteral("iconSource")).toString()}});
        }
        m_choices.insert(id);
        m_choiceModules.insert(id, modules);
        emit chooserRequested(id, item.value(QStringLiteral("intent")).toString(),
                              nameFor(item.value(QStringLiteral("requester")).toString()), providers);
    } else if (kind == QStringLiteral("present")) {
        // Raised only when what it went to has a screen: an app's backend.
        const QString app = appOf(item.value(QStringLiteral("provider")).toString());
        if (app.isEmpty()) return;
        m_navigatedAt.insert(item.value(QStringLiteral("dispatch")).toString(), m_clock.elapsed());
        if (m_presenter) m_presenter->presentApp(app);
    } else if (kind == QStringLiteral("completed")) {
        maybeReturn(item);
    } else if (kind == QStringLiteral("offer_install")) {
        emit installOffered(item.value(QStringLiteral("intent")).toString());
    } else if (kind == QStringLiteral("result")) {
        // Basecamp's own request settled.
        if (const ResultFn done = m_shellRequests.take(item.value(QStringLiteral("handle")).toString()))
            done(item.value(QStringLiteral("result")).toObject().toVariantMap());
    } else if (kind == QStringLiteral("dismiss")) {
        const QString work = item.value(QStringLiteral("work")).toString();
        m_choiceModules.remove(work);
        if (m_choices.remove(work)) emit chooserDismissed(work);
        if (m_shellDeliveries.remove(work)) emit shellDeliveryEnded(work);
    } else if (kind == QStringLiteral("pending")) {
        // At once for what ships with Basecamp; otherwise only when the person
        // just clicked in the asking app's screen, which a core module hasn't.
        const QString requester = item.value(QStringLiteral("requester")).toString();
        const bool atOnce = m_attention == QLatin1String("immediate")
            || (m_attention == QLatin1String("bundled") && m_requesterBundled
                && m_requesterBundled(requester));
        if (atOnce || takeInput(appOf(requester))) {
            answer(id, {{QStringLiteral("action"), QStringLiteral("open")}});
            return;
        }
        // No pending list yet: it waits in the module until it expires.
        qInfo().noquote() << "intents_module: pending request" << item.value(QStringLiteral("intent")).toString()
                          << "from" << requester;
    } else if (kind == QStringLiteral("deliver")) {
        m_shellDeliveries.insert(id);
        const int handlers = m_shellProvider
            ? m_shellProvider(id, item.value(QStringLiteral("intent")).toString(),
                              item.value(QStringLiteral("params")).toObject().toVariantMap(),
                              nameFor(item.value(QStringLiteral("requester")).toString()))
            : 0;
        if (handlers == 0) respondShell(id, false, QVariant(), QStringLiteral("unavailable"));
    }
}

void IntentsModuleHost::choose(const QString& id, const QString& shown)
{
    if (!m_choices.remove(id)) return;
    // Only one of the providers it offered: anything else starts nothing.
    const QString module = m_choiceModules.take(id).value(shown);
    if (module.isEmpty()) {
        qWarning().noquote() << "intents_module: chooser" << id << "was answered with" << shown
                             << "which it didn't offer; cancelling it";
        answer(id, {{QStringLiteral("action"), QStringLiteral("cancel")}});
        return;
    }
    const QJsonObject chosen{{QStringLiteral("action"), QStringLiteral("choose")},
                             {QStringLiteral("provider"), module}};
    // Answered once it runs, or once it could not start: delivering to it then
    // fails as `unavailable`, which is the truth.
    QPointer<IntentsModuleHost> self(this);
    start(module, [self, id, chosen]() {
        if (self) self->answer(id, chosen);
    });
}

void IntentsModuleHost::cancelChoice(const QString& id)
{
    m_choiceModules.remove(id);
    if (!m_choices.remove(id)) return;
    answer(id, {{QStringLiteral("action"), QStringLiteral("cancel")}});
}

void IntentsModuleHost::appLoadFailed(const QString& app)
{
    finishStart(app);
}

void IntentsModuleHost::start(const QString& provider, std::function<void()> done)
{
    const QString app = appOf(provider);
    if (!app.isEmpty()) startApp(app, std::move(done));
    else if (m_providerStarter) m_providerStarter(provider, std::move(done));
    else done();
}

// An app's backend comes up with its app: the pick waits for registerBackend().
void IntentsModuleHost::startApp(const QString& app, std::function<void()> done)
{
    const bool loaded = !m_presenter || m_presenter->isAppLoaded(app);
    // A closed app's backend went with it.
    if (!loaded) m_backends.remove(app);
    if (m_backends.contains(app)) {
        done();
        return;
    }
    QList<std::function<void()>>& waiting = m_starting[app];
    waiting.append(std::move(done));
    if (waiting.size() > 1) return;
    // Up already with no backend: nothing will come.
    if (loaded) {
        finishStart(app);
        return;
    }
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, app]() { finishStart(app); });
    m_startTimers.insert(app, timer);
    timer->start(m_startTimeoutMs);
    m_presenter->ensureAppLoaded(app);
}

void IntentsModuleHost::finishStart(const QString& app)
{
    if (QTimer* timer = m_startTimers.take(app)) timer->deleteLater();
    for (const std::function<void()>& done : m_starting.take(app)) done();
}

bool IntentsModuleHost::respondShell(const QString& id, bool ok, const QVariant& data, const QString& error,
                                     std::function<void(bool)> taken)
{
    if (!m_shellDeliveries.remove(id)) {
        if (taken) taken(false);
        return false;
    }
    answer(id,
           {{QStringLiteral("action"), QStringLiteral("respond")},
            {QStringLiteral("ok"), ok},
            {QStringLiteral("data"), QJsonValue::fromVariant(data)},
            {QStringLiteral("error"), error}},
           std::move(taken));
    return true;
}

// ── Auto-return ──────────────────────────────────────────────────────────────
// IntentBroker::maybeReturnToRequester's rules, on the module's `completed`
// item: back to the requester only when the provider answered, the request was
// no hand-off, the person is still on the provider, no dialog is up, and not
// faster than the eye can follow; a burst of returns pauses them.

bool IntentsModuleHost::mayReturn(const QString& requester, const QString& provider) const
{
    return m_presenter && !m_returnsPaused && provider != requester
        && m_presenter->isAppFrontmost(provider) && m_presenter->isAppLoaded(requester)
        && !m_presenter->anyDialogOpen();
}

void IntentsModuleHost::maybeReturn(const QJsonObject& completed)
{
    // Only a request that took the person somewhere brings them back.
    const auto navigated = m_navigatedAt.constFind(completed.value(QStringLiteral("dispatch")).toString());
    if (navigated == m_navigatedAt.cend()) return;
    const qint64 navigatedAt = navigated.value();
    m_navigatedAt.erase(navigated);
    if (m_returnsPaused && m_clock.elapsed() - m_returnsPausedAtMs >= m_breakerCooldownMs) {
        m_returnsPaused = false;
        m_returns.clear();
    }
    if (completed.value(QStringLiteral("by")).toString() != QStringLiteral("provider")
        || completed.value(QStringLiteral("handoff")).toBool())
        return;
    const QString requester = nameFor(completed.value(QStringLiteral("requester")).toString());
    const QString provider = nameFor(completed.value(QStringLiteral("provider")).toString());
    if (!mayReturn(requester, provider)) return;

    const qint64 dwell = m_clock.elapsed() - navigatedAt;
    if (dwell < m_dwellFloorMs) {
        QTimer::singleShot(int(m_dwellFloorMs - dwell), this,
                           [this, requester, provider, generation = m_generation]() {
            if (generation == m_generation && mayReturn(requester, provider))
                m_presenter->presentApp(requester);
        });
    } else {
        m_presenter->presentApp(requester);
    }
    noteReturn();
}

void IntentsModuleHost::noteReturn()
{
    const qint64 now = m_clock.elapsed();
    m_returns.append(now);
    while (!m_returns.isEmpty() && now - m_returns.first() > kReturnBreakerWindowMs)
        m_returns.removeFirst();
    if (!m_returnsPaused && m_returns.size() > kReturnBreakerLimit) {
        m_returnsPaused = true;
        m_returnsPausedAtMs = now;
        qWarning() << "intents_module:" << m_returns.size() << "auto-returns within"
                   << kReturnBreakerWindowMs << "ms — pausing auto-return for"
                   << (m_breakerCooldownMs / 1000) << "s";
    }
}
