#pragma once

#include <functional>
#include <memory>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class IntentBroker;
class IntentRegistry;
class ShellIntentEndpoint;

// ── LinkRequestCoordinator ───────────────────────────────────────────────────
//
// Drains LinkUrlInbox, parses, and submits to the broker as the link requester.
//
// THE GATE IS THE FIRST REGISTRY REBUILD, not the window appearing. A cold-start
// URL — the click that launched the app — arrives seconds before
// PackageCoordinator::refresh() has told IntentRegistry which apps provide what.
// Submitting in that window resolves to `unavailable` and, worse, may offer to
// install a package that is already there. So the URL waits in the inbox until
// onRegistryReady(), which is exactly the first successful rebuild.
//
// Everything else here is a bound:
//
//   - ONE IN FLIGHT. The broker QUEUES choosers rather than refusing them
//     (IntentBroker::startRequest), so a page navigating repeatedly would build
//     a queue of consent dialogs. Extra links are dropped, loudly.
//   - A PARK DEADLINE, so a URL waiting on a rebuild that never comes reports
//     failure instead of evaporating. Status has no equivalent: their stash is
//     dropped in silence if login never completes.
//
// Nothing is ever reported to the browser — there is no channel — so failures
// surface as linkFailed() and in the log.
class LinkRequestCoordinator : public QObject {
    Q_OBJECT
public:
    LinkRequestCoordinator(IntentBroker* broker,
                           IntentRegistry* registry,
                           QObject* parent = nullptr);
    ~LinkRequestCoordinator() override;

    // The module name this coordinator submits under. Registered with the
    // registry by the caller, and never registered as a provider.
    static QString requesterName();

    // Bare `basecamp://` raises the window and submits nothing.
    void setRaiseHandler(std::function<void()> raise);

    // Wired to the first successful IntentRegistry::rebuild().
    void onRegistryReady();

    // Test seam: the park deadline is minutes, and no test should pay it.
    void setParkDeadlineMs(int ms);

signals:
    // For the shell to show. `reason` is developer-facing text, never anything
    // an outside party supplied.
    void linkFailed(const QString& reason);

private:
    void drainInbox();
    void handleUrl(const QString& rawUrl);
    void onResult(const QString& requestId, const QVariantMap& envelope);
    void fail(const QString& reason);

    // QPointer, NOT a raw pointer, and this is load-bearing.
    //
    // Qt's QObjectPrivate::deleteChildren() walks the child list from index 0
    // — CONSTRUCTION order, not reverse, whatever the folklore says. The broker
    // is MainUIBackend's second child and this coordinator is its last, so by
    // the time ~LinkRequestCoordinator runs the broker is already freed. A raw
    // pointer here segfaulted every quit path in IntentBroker::endpointDestroyed,
    // reading a QHash through a dangling `this`.
    //
    // QPointer goes null the moment the QObject dies, so the destructor below
    // becomes a no-op in exactly that case while still unregistering properly
    // when the coordinator is the one that dies first.
    QPointer<IntentBroker>   m_broker;
    QPointer<IntentRegistry> m_registry;
    std::unique_ptr<ShellIntentEndpoint> m_endpoint;

    std::function<void()> m_raise;

    bool m_ready = false;
    bool m_inFlight = false;
    quint64 m_requestSerial = 0;

    QTimer m_parkDeadline;
    int m_parkDeadlineMs = 90 * 1000;
};
