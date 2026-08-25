#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include "IntentBroker.h"

class UIPluginManager;

// IntentPresenter over the shell's real app machinery — the adapter that keeps
// IntentBroker free of widgets, docks, sections and app-name special cases.
//
// WHY onAppLauncherClicked. It is the only primitive that both loads an unloaded
// app and activates a loaded one. activateApp() only raises an already-mounted
// widget, so an unloaded provider would hang to the activation deadline; and
// MainUIBackend::openApp() opens the install-resolver dialog unless the AppsModel
// row says Installed — a capability request must never become a purchase funnel.
//
// TWO KNOWN HOLES, stated rather than fixed: an `lgpm` install run outside
// basecamp emits no event and there is no filesystem watcher, so the registry
// misses it until something else refreshes; and an app can be uninstalled with
// its widget still mounted, so the registry may briefly disagree with reality.
class UIPluginPresenter : public QObject, public IntentPresenter {
    Q_OBJECT
public:
    explicit UIPluginPresenter(UIPluginManager* uiPluginManager,
                               QObject* parent = nullptr);

    // ── IntentPresenter ─────────────────────────────────────────────────
    bool isAppLoaded(const QString& appName) const override;
    void ensureAppLoaded(const QString& appName) override;
    void presentApp(const QString& appName) override;

private:
    QPointer<UIPluginManager> m_uiPluginManager;
};
