#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class IntentRegistry;
class QTimer;


// The broker reaches the world through these and nothing else, so it can be
// unit-tested against fakes with no bridge, no LogosAPI and no widgets.
// One app's QML surface, from the broker's point of view.
class IntentEndpoint {
public:
    virtual ~IntentEndpoint() = default;

    // Returns how many handlers received it. 0 means the app declared the
    // capability but is not listening — the one failure the frozen surface
    // cannot report any other way.
    virtual int deliverRequest(const QString& dispatchId,
                               const QString& intent,
                               const QVariantMap& params,
                               const QString& requesterName) = 0;
    virtual void deliverResult(const QString& requestId,
                               const QVariantMap& envelope) = 0;
    virtual QObject* asObject() = 0;
};

// Loading and showing an app. The broker knows nothing about widgets, docks or
// sections: only the shell knows where an app was mounted, so only the shell
// decides how to bring it forward.
class IntentPresenter {
public:
    virtual ~IntentPresenter() = default;

    virtual bool isAppLoaded(const QString& appName) const = 0;
    virtual void ensureAppLoaded(const QString& appName) = 0;
    virtual void presentApp(const QString& appName) = 0;
};

// A seam rather than a signal because the broker must know whether a chooser is
// actually MOUNTED. A signal cannot answer that — the shell re-emits it for QML,
// so the broker's receiver count is 1 either way. present() returns the real one.
class IntentChooser {
public:
    virtual ~IntentChooser() = default;

    // `providers` is a list of {moduleName, displayName, iconSource}, sorted.
    // 0 handlers means nothing is on screen and the broker must fail closed:
    // AwaitingChoice has no deadline by design.
    virtual int present(const QString& dispatchId,
                        const QString& intent,
                        const QString& requesterName,
                        const QVariantList& providers) = 0;

    virtual void dismiss(const QString& dispatchId) = 0;
};

// Suggesting a package for a capability nothing on disk can service.
//
// FIRE AND FORGET, AND NOT PART OF ANY REQUEST. The requester was already
// answered `unavailable` on the usual floor, exactly as it would have been with
// no catalog match at all, and cannot tell the two apart. Nothing to withdraw,
// nothing to report back.
class IntentInstaller {
public:
    virtual ~IntentInstaller() = default;

    // `candidates` are module names the catalog says could service the intent,
    // sorted. Nothing is returned: the request it came from is already finished.
    virtual void offerInstall(const QString& intent,
                              const QStringList& candidates) = 0;
};

// ── IntentBroker ─────────────────────────────────────────────────────────────
// Resolve a capability to a provider, present it, route the result back to the
// requester alone.
class IntentBroker : public QObject {
    Q_OBJECT
public:
    IntentBroker(IntentRegistry* registry,
                 IntentPresenter* presenter,
                 QObject* parent = nullptr);
    ~IntentBroker() override;

    // Setter injection, mirroring UIPluginManager::setPackageCoordinator.
    void setPresenter(IntentPresenter* presenter);
    void setChooser(IntentChooser* chooser);
    void setInstaller(IntentInstaller* installer);

    // The name->endpoint map is the broker's business; the runtime never learns
    // what an app is called.
    void registerEndpoint(const QString& appName, IntentEndpoint* endpoint);
    void unregisterEndpoint(IntentEndpoint* endpoint);

    // ── Called by the router adapter ────────────────────────────────────
    void submit(IntentEndpoint* from, const QString& requestId,
                const QString& intent, const QVariantMap& params);
    // Returns whether the response was accepted. False means the dispatch had
    // already ended (requester gone, timed out) and nobody heard the answer —
    // a provider that performed side effects needs to know that.
    bool submitResponse(IntentEndpoint* from, const QString& dispatchId,
                        bool ok, const QVariant& data, const QString& error);
    void abandon(IntentEndpoint* from, const QStringList& requestIds);
    void endpointDestroyed(IntentEndpoint* endpoint);

    // ── Called by the chooser ───────────────────────────────────────────
    //
    // It reports a DECISION and routes nothing itself, which is what lets it be
    // re-pointed at a runtime-supplied selection without touching the callers.
    void resolveChooser(const QString& dispatchId, const QString& providerName);
    void cancelChooser(const QString& dispatchId);

    // ── Called by the presenter when an app finishes loading ────────────
    void onAppReady(const QString& appName);
    void onAppUnavailable(const QString& appName);

    // Deadlines are overridable so CI does not have to wait out a 45 s
    // activation to prove the mute-provider case.
    void setTimeouts(int activationMs, int responseMs, int errorFloorMs);

    int pendingCount() const;

signals:
    // `providers` is a list of {moduleName, displayName, iconSource} maps,
    // sorted by moduleName. The shell draws the list; the requester never sees
    // it and cannot influence it.
    void chooserRequested(const QString& dispatchId,
                          const QString& intent,
                          const QString& requesterName,
                          const QVariantList& providers);
    void chooserDismissed(const QString& dispatchId);

private:
    enum class Phase {
        Accepted,
        AwaitingChoice,
        Activating,
        Dispatched
    };

    struct PendingRequest {
        QString dispatchId;
        QString requestId;                 // the requester's own id
        IntentEndpoint* requester = nullptr;
        QString requesterName;
        QString intent;
        QVariantMap params;

        IntentEndpoint* provider = nullptr;   // pointer identity for the guard
        QString providerName;

        Phase phase = Phase::Accepted;
        bool choicePresented = false;
        bool providerHadHandler = false;
        QVariantList choiceProviders;     // chooser payload
        qint64 acceptedAtMs = 0;
        qint64 phaseSinceMs = 0;
    };

    void startRequest(const QString& dispatchId);
    void chooseProvider(const QString& dispatchId, const QString& providerName);
    void dispatchTo(const QString& dispatchId);
    QString specViolation(const QString& providerName, const QString& intent,
                          const QVariantMap& params) const;
    void finish(const QString& dispatchId, bool ok, const QVariant& data,
                const QString& error);
    void failWithFloor(const QString& dispatchId, const QString& errorCode);
    void deliver(const PendingRequest& request, const QVariantMap& envelope);
    bool aChoiceIsOnScreen() const;
    void presentPendingChoice(const QString& dispatchId);
    void drainChoiceQueue();

    void sweep();
    void drainActivationQueue(const QString& appName, bool ready);
    QString nameForEndpoint(IntentEndpoint* endpoint) const;
    bool isShellProvider(const QString& providerName) const;
    qint64 nowMs() const;

    IntentRegistry* m_registry = nullptr;
    IntentPresenter* m_presenter = nullptr;
    IntentChooser*   m_chooser = nullptr;
    IntentInstaller* m_installer = nullptr;
    QTimer* m_sweepTimer = nullptr;
    QElapsedTimer m_clock;

    QHash<QString, PendingRequest> m_pending;         // dispatchId -> request
    QHash<IntentEndpoint*, QString> m_endpointNames;
    QHash<QString, IntentEndpoint*> m_endpointsByName;
    QHash<QString, QStringList> m_activationQueue;

    int m_activationTimeoutMs = 45000;
    int m_responseTimeoutMs = 20000;
    int m_errorFloorMs = 400;
};
