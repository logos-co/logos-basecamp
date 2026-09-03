#include "IntentBridgeAdapter.h"

#include "LogosIntent.h"
#include "LogosQmlBridge.h"

#include <QDebug>

// ── BridgeEndpoint ───────────────────────────────────────────────────────────

int IntentBridgeAdapter::BridgeEndpoint::deliverRequest(const QString& dispatchId,
                                                        const QString& intent,
                                                        const QVariantMap& params,
                                                        const QString& requesterName)
{
    if (!m_bridge) return 0;
    return m_bridge->deliverIntentRequest(dispatchId, intent, params, requesterName);
}

void IntentBridgeAdapter::BridgeEndpoint::deliverResult(const QString& requestId,
                                                        const QVariantMap& envelope)
{
    if (m_bridge)
        m_bridge->deliverIntentResult(requestId, envelope);
}

QObject* IntentBridgeAdapter::BridgeEndpoint::asObject()
{
    return m_bridge;
}

// ── IntentBridgeAdapter ──────────────────────────────────────────────────────

IntentBridgeAdapter::IntentBridgeAdapter(IntentBroker* broker, QObject* parent)
    : QObject(parent)
    , m_broker(broker)
{
}

IntentBridgeAdapter::~IntentBridgeAdapter()
{
    // Backstop for the destruction-order rule in the header: if this dies while
    // bridges are still alive, make sure none of them calls back into a freed
    // router.
    for (auto it = m_endpoints.begin(); it != m_endpoints.end(); ++it) {
        if (it.key())
            it.key()->setIntentRouter(nullptr);
        if (m_broker)
            m_broker->unregisterEndpoint(it.value());
        delete it.value();
    }
    m_endpoints.clear();
}

void IntentBridgeAdapter::attach(const QString& appName, LogosQmlBridge* bridge)
{
    if (!bridge || appName.isEmpty())
        return;

    if (m_endpoints.contains(bridge)) {
        qWarning() << "IntentBridgeAdapter: bridge already attached — ignoring"
                   << appName;
        return;
    }

    auto* endpoint = new BridgeEndpoint(bridge);
    m_endpoints.insert(bridge, endpoint);

    if (m_broker)
        m_broker->registerEndpoint(appName, endpoint);

    bridge->setIntentRouter(this);
}

void IntentBridgeAdapter::detach(LogosQmlBridge* bridge)
{
    auto it = m_endpoints.find(bridge);
    if (it == m_endpoints.end())
        return;

    if (bridge)
        bridge->setIntentRouter(nullptr);
    if (m_broker)
        m_broker->unregisterEndpoint(it.value());

    delete it.value();
    m_endpoints.erase(it);
}

IntentBridgeAdapter::BridgeEndpoint*
IntentBridgeAdapter::endpointFor(LogosQmlBridge* bridge) const
{
    return m_endpoints.value(bridge, nullptr);
}

void IntentBridgeAdapter::routeIntent(LogosQmlBridge* from, const QString& requestId,
                                      const QString& intent, const QVariantMap& params)
{
    BridgeEndpoint* endpoint = endpointFor(from);
    if (!endpoint || !m_broker) {
        // An unattached bridge has no identity, so it cannot have declared
        // anything. Answer exactly as a shell with no matching provider does.
        if (from)
            from->deliverIntentResult(requestId,
                                      logos::intent::makeEnvelope(
                                          false, QVariant(),
                                          logos::intent::errUnavailable()));
        return;
    }
    m_broker->submit(endpoint, requestId, intent, params);
}

void IntentBridgeAdapter::routeIntentResponse(LogosQmlBridge* from,
                                              const QString& requestId,
                                              bool ok, const QVariant& data,
                                              const QString& error)
{
    BridgeEndpoint* endpoint = endpointFor(from);
    if (!endpoint || !m_broker)
        return;   // silent: an unknown responder learns nothing
    m_broker->submitResponse(endpoint, requestId, ok, data, error);
}

void IntentBridgeAdapter::intentsAbandoned(LogosQmlBridge* from,
                                           const QStringList& requestIds)
{
    BridgeEndpoint* endpoint = endpointFor(from);
    if (!endpoint || !m_broker)
        return;
    m_broker->abandon(endpoint, requestIds);
}

void IntentBridgeAdapter::bridgeDestroyed(LogosQmlBridge* bridge)
{
    auto it = m_endpoints.find(bridge);
    if (it == m_endpoints.end())
        return;

    if (m_broker)
        m_broker->endpointDestroyed(it.value());

    delete it.value();
    m_endpoints.erase(it);
}
