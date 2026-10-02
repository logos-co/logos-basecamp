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
    using NothingFn = std::function<void(const QString& intent)>;

    explicit ShellIntentInstaller(OfferFn offer, NothingFn nothingInstallable = {})
        : m_offer(std::move(offer)), m_nothingInstallable(std::move(nothingInstallable)) {}

    void offerInstall(const QString& intent,
                      const QStringList& candidates) override
    {
        if (m_offer) m_offer(intent, candidates);
    }

    void nothingInstallable(const QString& intent) override
    {
        if (m_nothingInstallable) m_nothingInstallable(intent);
    }

private:
    OfferFn m_offer;
    NothingFn m_nothingInstallable;
};
