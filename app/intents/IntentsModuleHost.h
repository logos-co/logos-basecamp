#pragma once

#include "IntentBroker.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class QTimer;
class QWidget;

// Basecamp as intents_module's presenter: claims the module
// before any app loads, tells it what is installed, pulls its work, opens a
// request at once when what ships with Basecamp asks or the person just
// clicked in the asking app's screen, starts the provider the person picks,
// and provides the shell's own intents.
// The module knows modules; here an app's backend is shown as its app. The
// dialogs stay MainUIBackend's; this only says what to show and passes the
// person's answers back.
class IntentsModuleHost : public QObject {
    Q_OBJECT
public:
    // A synchronous call to intents_module, as the host. Returns its JSON reply,
    // or "" when the call failed.
    using Call = std::function<QString(const QString& method, const QVariantList& args)>;
    // One of the shell's own intents, delivered: how many handlers took it. 0
    // answers `unavailable`. The answer comes later, through respondShell().
    using ShellProvider = std::function<int(const QString& id, const QString& intent,
                                            const QVariantMap& params, const QString& requester)>;
    // A provider the person chose has to be running before it is delivered to.
    // `done` once it is, or once it could not be started. An app's backend
    // starts with its app, here; this is for any other module.
    using ProviderStarter = std::function<void(const QString& provider, std::function<void()> done)>;
    // The app `module` is the backend of; empty for any other module.
    using AppOfModule = std::function<QString(const QString& module)>;
    // Whether the package `module` comes from shipped with Basecamp.
    using RequesterBundled = std::function<bool(const QString& module)>;

    explicit IntentsModuleHost(Call call, QObject* parent = nullptr);

    // Loading, showing and probing apps; the old broker's seam.
    void setPresenter(IntentPresenter* presenter) { m_presenter = presenter; }
    void setShellProvider(ShellProvider provider) { m_shellProvider = std::move(provider); }
    void setProviderStarter(ProviderStarter starter) { m_providerStarter = std::move(starter); }
    void setAppOfModule(AppOfModule appOf) { m_appOfModule = std::move(appOf); }
    void setRequesterBundled(RequesterBundled bundled) { m_requesterBundled = std::move(bundled); }

    // Basecamp's own request, or a link's (`asLink`), through intents_module.
    // `done` gets the result envelope {ok, data, error} once it settles, or
    // failed if the module restarts first. False when refused at once, or
    // before the module holds the shell's declaration and what is installed;
    // `done` is not called then.
    using ResultFn = std::function<void(const QVariantMap& envelope)>;
    bool shellRequest(const QString& intent, const QVariantMap& params, bool asLink, ResultFn done = {});

    // "bundled" (the default): a request from what ships with Basecamp opens
    // at once, any other only when the person just clicked in the asking
    // app's screen, so another core module's never does. "interactive": every
    // request needs that click. "immediate": every request opens at once,
    // for developer builds only (intentsAttentionFor()). What doesn't open
    // waits in the pending list.
    void setAttention(const QString& attention) { m_attention = attention; }
    bool claim();
    bool isClaimed() const { return !m_secret.isEmpty(); }
    // Claimed, and the module took the shell's declaration and what is
    // installed. Both are sent again until it does: after 1 s, then twice as
    // long each time, up to a minute.
    bool isReady() const;
    void setSyncRetryMs(int firstMs, int capMs = 60 * 1000)
    {
        m_syncRetryMs = m_syncDelayMs = firstMs;
        m_syncRetryCapMs = capMs;
    }
    // intents_module was loaded again after it stopped. A new instance is
    // claimed and told everything again; the old one's dialogs close. If it
    // can't be claimed, presenterRoleLost().
    void restarted();

    // Whether intents_module, seen not running, should be loaded again: not
    // while a reload is under way, not once shutdown began, at most 3 times
    // a session. True takes the reload; reloadFinished() ends it. One that
    // fails leaves no module to ask until the next, on the next change to
    // what runs, and counts towards the 3; past the last, intents through
    // the module are off for the session.
    bool takeReload(bool moduleRunning);
    void reloadFinished(bool loaded);
    void shutdown() { m_shuttingDown = true; }

    void declareShell(const QJsonObject& shell);
    // [{ name, installDir, displayName, iconSource }]. Sent when it or a
    // package's <installDir>/metadata.json changed since the last time: the
    // module reads what each declares from there.
    void setInstalled(const QJsonArray& packages);
    // An app's backend is up, or stopped: what a pick of the app waits for.
    void registerBackend(const QString& app, const QString& module);
    void unregisterBackend(const QString& app);
    // An app finished loading. One started for a pick that brought up no
    // backend is answered anyway, and delivering to it fails.
    void appReady(const QString& app);

    // ── Clicks ──────────────────────────────────────────────────────────
    // Input in `view` is a click in `app`, for the next request its backend
    // makes. Every view an app gets (a rebuilt one too) is watched.
    void watchView(const QString& app, QWidget* view);
    // Posted events (the QML inspector's clicks) count too. Inspector builds only.
    void setAcceptSynthesizedInput(bool accept) { m_acceptSynthesized = accept; }
    void setInputWindowMs(int ms) { m_inputWindowMs = ms; }

    // ── Work ────────────────────────────────────────────────────────────
    // On work_available. Not before isReady(): the work waits in the module,
    // and is pulled once it is. A sync that failed is tried again at once.
    void pullWork();

    bool ownsChoice(const QString& id) const { return m_choices.contains(id); }
    void choose(const QString& id, const QString& provider);
    void cancelChoice(const QString& id);
    // An app started for a pick didn't load: the pick is answered, and
    // delivering to it fails as `unavailable`.
    void appLoadFailed(const QString& app);
    // How long a pick waits for its app to come up; tests shorten it.
    void setStartTimeoutMs(int ms) { m_startTimeoutMs = ms; }

    bool ownsShellDelivery(const QString& id) const { return m_shellDeliveries.contains(id); }
    // Answers a delivery to the shell, once. False when the module did not take
    // it: the request already ended, and nobody heard the answer.
    bool respondShell(const QString& id, bool ok, const QVariant& data, const QString& error);

    // Auto-return timings, as in IntentBroker; tests shorten them.
    void setReturnDwellFloorMs(int ms) { m_dwellFloorMs = ms; }
    void setReturnBreakerCooldownMs(int ms) { m_breakerCooldownMs = ms; }

signals:
    // `providers` as the shell's chooser takes them: {moduleName, displayName, iconSource}.
    void chooserRequested(const QString& id, const QString& intent,
                          const QString& requesterName, const QVariantList& providers);
    void chooserDismissed(const QString& id);
    // Nothing installed provides `intent`; the shell may offer one from its catalogue.
    void installOffered(const QString& intent);
    // A delivery to the shell ended without its answer: its dialog has to go.
    void shellDeliveryEnded(const QString& id);
    // A restarted intents_module couldn't be claimed: someone else may hold it,
    // so it must not keep running.
    void presenterRoleLost();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QJsonObject callJson(const QString& method, const QVariantList& args) const;
    // The app a module is the backend of, else empty; nameFor() falls back to the module.
    QString appOf(const QString& module) const;
    QString nameFor(const QString& module) const;
    bool takeInput(const QString& app);
    void start(const QString& provider, std::function<void()> done);
    void startApp(const QString& app, std::function<void()> done);
    void finishStart(const QString& app);
    bool answer(const QString& workId, const QJsonObject& answer);
    void take(const QJsonArray& items);
    void handle(const QJsonObject& item);
    void forgetInstance();
    void sync();
    void maybeReturn(const QJsonObject& completed);
    bool mayReturn(const QString& requester, const QString& provider) const;
    void noteReturn();

    Call m_call;
    QString m_attention = QStringLiteral("bundled");
    QString m_secret;
    QElapsedTimer m_clock;
    IntentPresenter* m_presenter = nullptr;
    ShellProvider m_shellProvider;
    ProviderStarter m_providerStarter;
    AppOfModule m_appOfModule;
    bool m_acceptSynthesized = false;
    int m_inputWindowMs = 2000;
    int m_startTimeoutMs = 45 * 1000;

    // What the module was told, to tell a restarted one again.
    QJsonObject m_shell;
    bool m_shellDeclared = false;
    bool m_shellAcked = false;
    QJsonArray m_installed;
    QStringList m_installedRevisions;   // each package's metadata.json, as it was sent
    bool m_installedSent = false;
    bool m_installedAcked = false;
    QTimer* m_syncTimer = nullptr;
    int m_syncRetryMs = 1000;
    int m_syncRetryCapMs = 60 * 1000;
    int m_syncDelayMs = 1000;           // before the next attempt, if this one fails
    bool m_workWaiting = false;         // pulled once ready
    bool m_reclaimed = false;           // "claimed again" is said once ready
    QJsonArray m_heldWork;              // pulled before ready, handled once it is
    RequesterBundled m_requesterBundled;
    // Which instance delayed work belongs to; a new one drops the old's.
    quint64 m_generation = 0;
    // Lost the presenter role: intents_module is not loaded again.
    bool m_gaveUp = false;
    QHash<QString, ResultFn> m_shellRequests;   // handle -> who waits for its result
    QHash<QString, QString> m_backends;          // app -> its running backend

    QHash<const QObject*, QString> m_appOfView;  // watched view -> its app
    QHash<QString, qint64> m_inputAtMs;          // app -> its unspent click
    QSet<QString> m_choices;                     // chooser work ids on screen
    QHash<QString, QHash<QString, QString>> m_choiceModules;   // chooser -> shown name -> module
    QSet<QString> m_shellDeliveries;             // deliver work ids the shell owes an answer
    QHash<QString, QList<std::function<void()>>> m_starting;   // app -> picks waiting for it
    QHash<QString, QTimer*> m_startTimers;       // app -> when to give up waiting
    QHash<QString, qint64> m_navigatedAt;        // dispatch -> when its provider was presented

    bool m_reloading = false;
    int m_reloads = 0;
    bool m_shuttingDown = false;

    QList<qint64> m_returns;
    bool m_returnsPaused = false;
    qint64 m_returnsPausedAtMs = 0;
    int m_dwellFloorMs = 400;
    qint64 m_breakerCooldownMs = 30 * 1000;
};
