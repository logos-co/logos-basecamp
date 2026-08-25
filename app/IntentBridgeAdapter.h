#pragma once

#include "IntentBroker.h"
#include "LogosIntentRouter.h"

#include <QHash>
#include <QObject>
#include <QString>

class LogosQmlBridge;

// ── IntentBridgeAdapter ──────────────────────────────────────────────────────
//
// The only place that knows about both LogosQmlBridge and IntentBroker, so the
// broker stays free of the runtime's headers and the runtime free of basecamp's.
// Pass-through only: no policy, no state beyond the endpoint map.
//
// It also holds the one thing the runtime refuses to — which bridge is which
// app. The runtime uses bridge POINTERS as identity; the name lives here.
//
// ⚠ DESTRUCTION ORDER. Must be constructed BEFORE anything owning bridges
// (UIPluginManager) so Qt's reverse-order destruction kills it after them;
// otherwise a bridge destructor calls bridgeDestroyed() on freed memory. The
// destructor also nulls the router on every bridge it still knows, so the rule
// has a backstop rather than only a comment.
class IntentBridgeAdapter : public QObject, public LogosIntentRouter {
    Q_OBJECT
public:
    IntentBridgeAdapter(IntentBroker* broker, QObject* parent = nullptr);
    ~IntentBridgeAdapter() override;

    // Bind a freshly created bridge to an app name and point it at this router.
    void attach(const QString& appName, LogosQmlBridge* bridge);
    void detach(LogosQmlBridge* bridge);

    // ── LogosIntentRouter ───────────────────────────────────────────────
    void routeIntent(LogosQmlBridge* from, const QString& requestId,
                     const QString& intent, const QVariantMap& params) override;
    void routeIntentResponse(LogosQmlBridge* from, const QString& requestId,
                             bool ok, const QVariant& data,
                             const QString& error) override;
    void intentsAbandoned(LogosQmlBridge* from, const QStringList& requestIds) override;
    void bridgeDestroyed(LogosQmlBridge* bridge) override;

private:
    // Adapts one bridge to the broker's endpoint seam.
    class BridgeEndpoint : public IntentEndpoint {
    public:
        explicit BridgeEndpoint(LogosQmlBridge* bridge) : m_bridge(bridge) {}

        int deliverRequest(const QString& dispatchId, const QString& intent,
                           const QVariantMap& params,
                           const QString& requesterName) override;
        void deliverResult(const QString& requestId,
                           const QVariantMap& envelope) override;
        QObject* asObject() override;

        LogosQmlBridge* bridge() const { return m_bridge; }

    private:
        LogosQmlBridge* m_bridge = nullptr;
    };

    BridgeEndpoint* endpointFor(LogosQmlBridge* bridge) const;

    IntentBroker* m_broker = nullptr;
    QHash<LogosQmlBridge*, BridgeEndpoint*> m_endpoints;
};
