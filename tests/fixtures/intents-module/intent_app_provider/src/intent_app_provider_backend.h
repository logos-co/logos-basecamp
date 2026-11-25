#pragma once

#include "rep_intent_app_provider_source.h"
#include "logos_module_context.h"

#include <QString>

// Provides test.app_echo: intents_module delivers the request here, the view
// shows it, and the person's Approve or Decline is the answer.
class IntentAppProviderBackend : public IntentAppProviderSimpleSource,
                                 public LogosModuleContext
{
public:
    // intents_module's delivery (the generated glue lets no one else call it).
    QString deliver(const QString& dispatch, const QString& intent,
                    const QString& paramsJson, const QString& requester);

    void approve() override;
    void decline() override;

private:
    void respond(bool ok, const QString& dataJson, const QString& error);

    QString m_dispatch;
    QString m_text;
    QString m_requester;
};
