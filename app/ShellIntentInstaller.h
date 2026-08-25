#pragma once

#include <functional>

#include <QString>
#include <QStringList>

#include "IntentBroker.h"

// The shell's "install a provider?" suggestion, as the broker sees it.
//
// Callable-injected like ShellIntentChooser, but with nothing to report back:
// the request was already answered `unavailable`, so a prompt failing to mount
// is not a case to recover from.
//
// THE SHELL OWNS THIS, not the requesting app. Telling the app "not installed"
// and letting it decide would hand every app an oracle for the user's installed
// list — exactly what the merged `unavailable` exists to prevent. So the shell
// discovers, asks and installs; the user retries and it resolves normally.
class ShellIntentInstaller : public IntentInstaller {
public:
    using OfferFn = std::function<void(const QString& intent,
                                       const QStringList& candidates)>;

    explicit ShellIntentInstaller(OfferFn offer) : m_offer(std::move(offer)) {}

    void offerInstall(const QString& intent,
                      const QStringList& candidates) override
    {
        if (m_offer) m_offer(intent, candidates);
    }

private:
    OfferFn m_offer;
};
