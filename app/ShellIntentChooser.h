#pragma once

#include <functional>

#include <QString>
#include <QVariantList>

#include "IntentBroker.h"

// The shell's chooser, as the broker sees it.
//
// Callable-injected like ShellIntentEndpoint: the receiver count of the
// QML-facing signal is the only honest answer to "is a chooser on screen", and
// only MainUIBackend can compute it. Counting receivers on the broker instead
// would always report 1 — the shell connects that signal to re-emit it — and
// AwaitingChoice has no deadline, so the request would hang forever.
class ShellIntentChooser : public IntentChooser {
public:
    using PresentFn = std::function<int(const QString& dispatchId,
                                        const QString& intent,
                                        const QString& requesterName,
                                        const QVariantList& providers)>;
    using DismissFn = std::function<void(const QString& dispatchId)>;

    ShellIntentChooser(PresentFn present, DismissFn dismiss)
        : m_present(std::move(present)), m_dismiss(std::move(dismiss)) {}

    int present(const QString& dispatchId, const QString& intent,
                const QString& requesterName,
                const QVariantList& providers) override
    {
        return m_present ? m_present(dispatchId, intent, requesterName, providers) : 0;
    }

    void dismiss(const QString& dispatchId) override
    {
        if (m_dismiss) m_dismiss(dispatchId);
    }

private:
    PresentFn m_present;
    DismissFn m_dismiss;
};
