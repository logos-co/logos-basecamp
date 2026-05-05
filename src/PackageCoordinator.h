#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <QMap>
#include "logos_api.h"

class CoreModuleManager;
class UIPluginManager;

// PackageCoordinator — owns every interaction with the `package_manager` LogosAPI
// module.
//
// Scope:
//   * Package scanning (getInstalledPackagesAsync, getInstalledUiPluginsAsync)
//     and the derived installType / missing-deps / dependents caches.
//   * Install orchestration — inspectPackage → install-confirm dialog →
//     installPluginAsync (with the "upgrade with dependents" cascade branch).
//   * Gated uninstall/upgrade flow — requestUninstall / ackPendingAction /
//     confirmUninstall / cancelUninstall (and the upgrade siblings), plus the
//     cascade-unload + confirm handshake driven by beforeUninstall /
//     beforeUpgrade events.
//   * Pending-action state for the package-lifecycle cascade (one slot for
//     UninstallCascade / UpgradeCascade / InstallUpgradeCascade).
//
// What it does NOT do:
//   * Mount or unmount UI plugin widgets. UI lifecycle lives in
//     UIPluginManager; when a cascade needs to tear down a UI widget or check
//     whether something is loaded in this process, PackageCoordinator calls into
//     UIPluginManager via the pointer injected through setUIPluginManager.
//   * Touch the logos_core_* C API. Cascade unloads go through
//     CoreModuleManager.
class PackageCoordinator : public QObject {
    Q_OBJECT

public:
    // coreModuleManager and uiPluginManager are NOT owned — they're sibling
    // Qt children of the same MainUIBackend. Construction order is:
    //   CoreModuleManager → UIPluginManager → PackageCoordinator
    // so both pointers are valid by the time this ctor fires. They outlive
    // PackageCoordinator via Qt's reverse-order child destruction.
    explicit PackageCoordinator(LogosAPI* logosAPI,
                            CoreModuleManager* coreModuleManager,
                            UIPluginManager* uiPluginManager,
                            QObject* parent = nullptr);
    ~PackageCoordinator() override;

    // Read-only accessors over the package-state caches. Empty when the
    // async refresh chain hasn't completed yet; QML and UIPluginManager
    // are expected to treat "empty" as "not known — show safe defaults".
    QString     installType(const QString& name) const;
    QStringList missingDepsOf(const QString& name) const;
    QStringList dependentsOf(const QString& name) const;

public slots:
    // Install confirmation flow. installPluginFromPath inspects the LGX,
    // branches between fresh-install and upgrade-with-dependents, and
    // emits installConfirmationRequested. QML drives the dialog, then
    // calls confirmInstall / cancelInstall.
    Q_INVOKABLE void installPluginFromPath(const QString& filePath);
    Q_INVOKABLE void openInstallPluginDialog();
    Q_INVOKABLE void confirmInstall();
    Q_INVOKABLE void cancelInstall();

    // Gated uninstall. Both slots kick off requestUninstallAsync — the
    // module owns its own pending state and emits beforeUninstall which
    // we handle in onBeforeUninstall. uninstallUiModule additionally
    // refuses "main_ui" because uninstalling it would brick Basecamp.
    Q_INVOKABLE void uninstallUiModule(const QString& moduleName);
    Q_INVOKABLE void uninstallCoreModule(const QString& moduleName);

    // Cascade confirmation — called from QML once the user OKs the
    // uninstall / upgrade / local-upgrade dialog. Dispatches to the right
    // confirmX / uninstallPackage + installPlugin chain based on which
    // PendingOp the current state holds.
    Q_INVOKABLE void confirmUninstallCascade(const QString& moduleName);

    // Multi-uninstall counterparts. Same gated protocol as the single-package
    // path but for the batch initiated by package_manager.requestMultiUninstall.
    // confirm runs the cascade-unload for every name in the batch and then
    // calls confirmMultiUninstall. cancel forwards to cancelMultiUninstall.
    Q_INVOKABLE void confirmUninstallMultiCascade(const QStringList& moduleNames);
    Q_INVOKABLE void cancelMultiUninstall(const QStringList& moduleNames);

    // Cancel counterpart — a no-op when the pending action's name doesn't
    // match (MainUIBackend fans out cancelPendingAction to both managers so
    // one of them will always be a no-op).
    Q_INVOKABLE void cancelPendingAction(const QString& moduleName);

    // Full rescan of the package catalog. Fires the same fetchUiPluginMetadata
    // → refreshDependencyInfo chain the file-install event subscriptions
    // trigger — used by the UI Modules tab's Reload button (forwarded from
    // UIPluginManager::refreshUiModules) and by MainUIBackend right after
    // construction to do the first-time catalog load once all three managers
    // are wired. Kept as a public slot rather than running from the ctor so
    // the initial uiPluginsFetched signal isn't emitted before listeners have
    // had a chance to connect.
    Q_INVOKABLE void refresh();

signals:
    // Tells MainUIBackend to refresh the uiModules / launcherApps / coreModules
    // properties — their values compose installType/missing-deps from here with
    // loaded-state from UIPluginManager and CoreModuleManager.
    void uiModulesChanged();
    void launcherAppsChanged();
    void coreModulesChanged();

    // Raw UI-plugin metadata pushed to UIPluginManager whenever the catalog
    // refreshes. UIPluginManager owns the UI-plugin-specific cache
    // (m_uiPluginMetadata) because that's where the load-dispatch path reads
    // from; PackageCoordinator is just the IPC edge that knows when the data has
    // changed.
    void uiPluginsFetched(const QVariantList& uiPlugins);

    // Install-confirmation dialog trigger — QML hosts the dialog; metadata
    // shape matches the inspectPackage result plus an optional
    // loadedDependents list for the "upgrade with dependents" branch.
    void installConfirmationRequested(const QVariantMap& metadata);

    // Uninstall/upgrade cascade dialog trigger. Shared shape — the dialog
    // is the same whether we're about to remove a package or swap versions;
    // the copy ("these depend on X") applies either way.
    void uninstallCascadeConfirmationRequested(const QString& name,
                                               const QStringList& installedDependents,
                                               const QStringList& loadedDependents);

    // Multi-uninstall cascade dialog trigger. `names` is the full batch of
    // packages being uninstalled. `installedDependents` is the union of each
    // name's recursive reverse dependents minus the names already in the batch
    // (the module computed it that way; we just pass through). `loadedDependents`
    // is the subset of installedDependents currently running.
    void uninstallMultiCascadeConfirmationRequested(const QStringList& names,
                                                    const QStringList& installedDependents,
                                                    const QStringList& loadedDependents);

private slots:
    // beforeUninstall / beforeUpgrade handlers. Both ack synchronously (to
    // cancel the module's 3s ack timer) then — if the ack landed — set the
    // pending slot here and emit uninstallCascadeConfirmationRequested. An
    // ack rejection means the module already cancelled (timer fired or
    // racing listener), so we stay silent rather than showing a dead dialog.
    void onBeforeUninstall(const QString& name, const QStringList& installedDeps);
    void onBeforeUpgrade(const QString& name, const QString& releaseTag,
                         int mode, const QStringList& installedDeps);

    // Multi-uninstall variant — same ack-then-emit-dialog shape, but holds the
    // batch's full name list in m_pendingAction.names so confirm/cancel can
    // forward the same list back to the module.
    void onBeforeMultiUninstall(const QStringList& names, const QStringList& installedDeps);

private:
    // The gated-cascade pending slot. UnloadCascade (local, no IPC) lives on
    // UIPluginManager. Here we only track the ops that the package_manager
    // module itself gates (uninstall / upgrade) plus the local LGX-upgrade
    // variant that shares the cascade dialog but runs without module-side
    // gating (it's just uninstallPackage + installPlugin chained).
    enum class PendingOp { None, UninstallCascade, UpgradeCascade, InstallUpgradeCascade, MultiUninstallCascade };
    struct PendingAction {
        PendingOp op = PendingOp::None;
        QString   name;
        QString   releaseTag;       // UpgradeCascade only
        int       upgradeMode = 0;  // UpgradeCascade only
        QStringList names;          // MultiUninstallCascade only — full batch (kept last so existing positional initialisers stay valid)
    };

    // Subscribe to corePluginFileInstalled/uiPluginFileInstalled/
    // corePluginUninstalled/uiPluginUninstalled + beforeUninstall/beforeUpgrade.
    // Also configures install directories on the module and issues
    // resetPendingActionAsync to clear any slot left over from a crashed prior
    // session.
    void subscribeToPackageInstallationEvents();

    // Pull UI plugin metadata from the module and emit uiPluginsFetched. Also
    // seeds the installType cache for the UI-plugin subset; the full-scan pass
    // in refreshDependencyInfo overwrites it with the core-inclusive version.
    void fetchUiPluginMetadata();

    // Full rescan: getInstalledPackages → per-entry resolveFlatDependencies +
    // resolveFlatDependents. Populates m_installTypeByModule,
    // m_missingDepsByModule, m_dependentsByModule. Emits uiModulesChanged +
    // coreModulesChanged + launcherAppsChanged when done so the QML bindings
    // pick up the new installType / missing-deps values.
    void refreshDependencyInfo();

    // Wiring (not owned — see ctor comment).
    LogosAPI*          m_logosAPI;
    CoreModuleManager* m_coreModuleManager;
    UIPluginManager*   m_uiPluginManager;

    // Package-state caches sourced from the package_manager module.
    QMap<QString, QString>     m_installTypeByModule;
    QMap<QString, QStringList> m_missingDepsByModule;
    QMap<QString, QStringList> m_dependentsByModule;

    // Cascade-confirmation pending state (single-slot). See PendingOp above
    // for which ops land here.
    PendingAction m_pendingAction;

    // LGX path pending user confirmation from the install dialog. Set by
    // installPluginFromPath after a successful inspect, consumed by
    // confirmInstall or by the InstallUpgradeCascade branch of
    // confirmUninstallCascade. Empty when no install is pending.
    QString m_pendingInstallPath;
};
