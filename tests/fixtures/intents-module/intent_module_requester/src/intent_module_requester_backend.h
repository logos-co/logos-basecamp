#pragma once

#include "rep_intent_module_requester_source.h"
#include "logos_module_context.h"

#include <QList>
#include <QString>

// Asks intents_module for an intent, right on a click in its view or later
// (`clicked` only labels which), and keeps each request's outcome:
// `status` is the newest, `history` the latest few, `waiting` how many have no
// answer yet.
class IntentModuleRequesterBackend : public IntentModuleRequesterSimpleSource,
                                     public LogosModuleContext
{
public:
    void ask(QString intent, QString text, bool clicked) override;
    void cancelWaiting() override;
    // Calls a provider's deliver() itself, not through intents_module: it must refuse.
    void deliverDirectly(QString module, QString intent) override;

protected:
    void onContextReady() override;

private:
    struct Request {
        int number = 0;
        bool clicked = false;
        QString handle;
        QString receipt;
        QString outcome;
    };

    Request* find(int number);
    void fetchResult(const QString& handle);
    void setOutcome(int number, const QString& outcome);
    void publish();

    QList<Request> m_requests;
    int m_next = 0;
};
