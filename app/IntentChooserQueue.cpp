#include "IntentChooserQueue.h"

void IntentChooserQueue::request(const Chooser& chooser)
{
    if (chooser.dispatchId.isEmpty() || chooser.dispatchId == m_onScreen) return;
    for (const Chooser& c : m_waiting)
        if (c.dispatchId == chooser.dispatchId) return;
    m_waiting.append(chooser);
    if (m_onScreen.isEmpty()) next();
}

void IntentChooserQueue::withdraw(const QString& dispatchId)
{
    if (dispatchId.isEmpty()) return;
    if (dispatchId == m_onScreen) {
        emit close(dispatchId);
        return;
    }
    for (int i = 0; i < m_waiting.size(); ++i) {
        if (m_waiting.at(i).dispatchId == dispatchId) {
            m_waiting.removeAt(i);
            return;
        }
    }
}

void IntentChooserQueue::closed(const QString& dispatchId)
{
    if (dispatchId.isEmpty() || dispatchId != m_onScreen) return;
    m_onScreen.clear();
    next();
}

void IntentChooserQueue::next()
{
    if (m_waiting.isEmpty()) return;
    const Chooser c = m_waiting.takeFirst();
    m_onScreen = c.dispatchId;
    emit show(c.dispatchId, c.intent, c.requesterName, c.providers);
}
