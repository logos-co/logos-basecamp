#pragma once

#include <functional>

#include <QString>
#include <QVariantMap>

#include "IntentBroker.h"

// The shell itself, as an intent endpoint.
//
// main_ui provides capabilities like `basecamp.repositories.manage` that operate on
// the shell's own chrome. To the broker it is just another provider — one
// dispatch path, and no privilege earned from being the host.
//
// Delivery is a callable rather than a Qt signal so this needs no moc, and so
// MainUIBackend controls the receiver count. That count is the only way the
// frozen surface can report "declared the capability, nothing is listening".
class ShellIntentEndpoint : public IntentEndpoint {
public:
    using DeliverFn = std::function<int(const QString& dispatchId,
                                        const QString& intent,
                                        const QVariantMap& params,
                                        const QString& requesterName)>;
    using ResultFn = std::function<void(const QString& requestId,
                                        const QVariantMap& envelope)>;

    using EndedFn = std::function<void(const QString& dispatchId)>;

    explicit ShellIntentEndpoint(DeliverFn deliver, ResultFn onResult = {})
        : m_deliver(std::move(deliver)), m_onResult(std::move(onResult)) {}

    // A request the shell took ended without its answer.
    void setOnEnded(EndedFn onEnded) { m_onEnded = std::move(onEnded); }

    int deliverRequest(const QString& dispatchId, const QString& intent,
                       const QVariantMap& params,
                       const QString& requesterName) override
    {
        return m_deliver ? m_deliver(dispatchId, intent, params, requesterName) : 0;
    }

    void deliverResult(const QString& requestId,
                       const QVariantMap& envelope) override
    {
        if (m_onResult) m_onResult(requestId, envelope);
    }

    void dispatchEnded(const QString& dispatchId) override
    {
        if (m_onEnded) m_onEnded(dispatchId);
    }

    // Null is correct, not a shortcut: asObject() tracks endpoints that can
    // outlive the broker's interest, and this one dies with the shell.
    QObject* asObject() override { return nullptr; }

private:
    DeliverFn m_deliver;
    ResultFn  m_onResult;
    EndedFn   m_onEnded;
};
