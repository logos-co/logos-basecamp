#pragma once

#include "InstallEnums.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <QMap>
#include <QSet>
#include "logos_api.h"

class AppsFilterProxy;
class AppsModel;
class InstallRegistry;
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
                            AppsModel* appsModel,
                            QObject* parent = nullptr);
    ~PackageCoordinator() override;

    void setRequiredPackagesModel(AppsFilterProxy* proxy) { m_requiredPackagesModel = proxy; }

    // Read-only accessors over the package-state caches. Empty when the
    // async refresh chain hasn't completed yet; QML and UIPluginManager
    // are expected to treat "empty" as "not known — show safe defaults".
    QString     installType(const QString& name) const;
    QStringList missingDepsOf(const QString& name) const;
    QStringList dependentsOf(const QString& name) const;
    QString     displayNameFor(const QString& name) const;

    // Last-known package_downloader repository list, refreshed on demand via
    // refreshRepositories() and after every successful add/remove/toggle.
    QVariantList repositories() const { return m_repositories; }
    bool repositoriesLoading() const { return m_repositoriesLoadingCount > 0; }

    // True until the first successful populateAppsModel() — drives the
    // App Manager's loading placeholder. Stays false on subsequent refreshes
    // so a background catalog re-fetch doesn't wipe the visible grid.
    bool appsLoading() const { return m_appsLoading; }

public slots:
    // Install confirmation flow. installPluginFromPath inspects the LGX,
    // branches between fresh-install and upgrade-with-dependents, and
    // emits installConfirmationRequested. QML drives the dialog, then
    // calls confirmInstall / cancelInstall.
    Q_INVOKABLE void installPluginFromPath(const QString& filePath);
    Q_INVOKABLE void openInstallPluginDialog();
    Q_INVOKABLE void confirmInstall();
    Q_INVOKABLE void cancelInstall();

    Q_INVOKABLE void openApp(const QString& name,
                             const QString& repositoryUrl,
                             const QVariantMap& versionPins = QVariantMap(),
                             bool allowFastLaunch = true);
    Q_INVOKABLE void confirmCatalogInstall(const QString& name,
                                           const QString& repositoryUrl,
                                           const QVariantMap& versionPins = QVariantMap());

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

    // Package-repository management — thin wrappers around the
    // package_downloader IPC surface
    Q_INVOKABLE void refreshRepositories();
    Q_INVOKABLE void addRepository(const QString& url);
    Q_INVOKABLE void removeRepository(const QString& url);
    Q_INVOKABLE void setRepositoryEnabled(const QString& url, bool enabled);

    // Called by QML when the Add Application dialog closes so stale async
    // resolver callbacks for previously-opened apps don't mutate the shared
    // required-packages model or reopen the dialog.
    Q_INVOKABLE void notifyAddApplicationDialogClosed();

    InstallRegistry* installRegistry() const { return m_installRegistry; }

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

    // Sync entry from openApp() only — QML opens the modal if not already
    // visible (or refreshes in place when re-resolving the same app).
    void requestOpenAddApplicationDialog(const QVariantMap& metadata);
    // Passive refresh — never opens the modal; stale callbacks are dropped in
    // emitDialogMetadata before this is emitted.
    void addApplicationDataUpdated(const QVariantMap& metadata);
    void catalogInstallStageChanged(const QString& name, InstallStage::Value stage);
    void catalogInstallFinished(const QString& name);
    void catalogInstallFailed(const QString& name, const QString& error);
    void launchAppRequested(const QString& name);

    // Install-confirmation dialog trigger — QML hosts the dialog; metadata
    // shape matches the inspectPackage result plus an optional
    // loadedDependents list for the "upgrade with dependents" branch.
    void installConfirmationRequested(const QVariantMap& metadata);

    // Uninstall cascade dialog trigger — the "destructive" variant. Used
    // for beforeUninstall (a real removal) and InstallUpgradeCascade (LGX
    // upgrade that uninstalls first; the new version is local, not from
    // the catalog, so we don't know its version string to surface here).
    void uninstallCascadeConfirmationRequested(const QString& name,
                                               const QStringList& installedDependents,
                                               const QStringList& loadedDependents);

    // Upgrade/Downgrade/Reinstall cascade dialog trigger. Same dependent-
    // impact lists as the uninstall variant (the package_manager performs
    // an uninstall step first), but carries the target version + the
    // UpgradeMode so the dialog can lead with "Upgrade to v1.2.3" /
    // "Downgrade to v1.0.0" / "Reinstall v1.0.0" instead of bare
    // "Uninstall and Unload Dependents?". `mode` mirrors
    // PackageTypes::UpgradeMode (0=Upgrade, 1=Downgrade, 2=Sidegrade).
    void upgradeCascadeConfirmationRequested(const QString& name,
                                             const QString& releaseTag,
                                             int mode,
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

    // Repository management — change-notify for the QML-facing cache and
    // an outcome signal for add/remove/toggle (success or error string).
    void repositoriesChanged();
    void repositoriesLoadingChanged();
    void appsLoadingChanged();
    void repositoryOperationCompleted(const QString& operation,
                                      const QString& url,
                                      bool success,
                                      const QString& error);

private slots:
    // beforeUninstall / beforeUpgrade handlers. Both ack synchronously (to
    // cancel the module's 3s ack timer) then — if the ack landed — set the
    // pending slot here and emit the appropriate cascade-confirmation
    // signal: uninstallCascadeConfirmationRequested for a real removal,
    // upgradeCascadeConfirmationRequested for a version swap (so the
    // dialog can lead with the target version + UpgradeMode instead of
    // bare "Uninstall and Unload Dependents?"). An ack rejection means
    // the module already cancelled (timer fired or racing listener), so
    // we stay silent rather than showing a dead dialog.
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

    // Subscribe to package_downloader's catalogChanged event
    void subscribeToPackageDownloaderEvents();

    // Pull UI plugin metadata from the module and emit uiPluginsFetched. Also
    // seeds the installType cache for the UI-plugin subset; the full-scan pass
    // in refreshDependencyInfo overwrites it with the core-inclusive version.
    void fetchUiPluginMetadata();

    void tryFetchCatalog(const QHash<QString, QString>& installedByName, int retriesLeft);
    void buildCatalogIndexes(const QVariantList& catalog);
    void populateAppsModel(const QVariantList& catalog,
                           const QHash<QString, QString>& installedByName);

    // Full rescan: getInstalledPackages → per-entry resolveFlatDependencies +
    // resolveFlatDependents. Populates m_installTypeByModule,
    // m_missingDepsByModule, m_dependentsByModule. Emits uiModulesChanged +
    // coreModulesChanged + launcherAppsChanged when done so the QML bindings
    // pick up the new installType / missing-deps values.
    void refreshDependencyInfo();

    // Builds the resolver's depsJson for `name@repositoryUrl` with optional
    // per-row version pins. The target is row 0; remaining pin rows fall back
    // to the catalog-known repo from m_repoByName. Empty version/repo fields
    // are omitted so the resolver uses its newest/cross-repo defaults.
    QString buildResolverDepsJson(const QString& name,
                                  const QString& repositoryUrl,
                                  const QVariantMap& versionPins) const;
    // Transitive required-package set ({name, repositoryUrl}) computed purely
    // from the local catalog dependency graph — no async resolver.
    QVariantList collectCatalogRequired(const QString& name,
                                        const QString& repositoryUrl) const;
    QString buildInstalledPackagesJson() const;
    QVariantList computeDepChanges(const QVariantList& resolved,
                                   const QHash<QString, QString>& installedByName) const;
    static QString depAction(const QString& installedVersion,
                             const QString& resolvedVersion,
                             const QString& installedHash,
                             const QString& resolvedHash);
    static QVariantMap changeFromResolverEntry(const QVariantMap& entry,
                                               const QString& installedVersion,
                                               const QString& installedHash);
    static bool installPluginSucceeded(const QVariantMap& installResult);

    void runResolverAndOpenDialog(const QString& name,
                                  const QString& repositoryUrl,
                                  const QVariantMap& versionPins);
    void emitDialogMetadata(const QString& name,
                            const QString& repositoryUrl,
                            const QString& targetVersion,
                            const QVariantMap& catalogRow,
                            const QVariantList& changes,
                            bool requestOpen);
    // Recompute resolver overlay from cached raw resolve + current disk state.
    // Keeps dep badges correct after the install registry is cleared.
    void refreshOverlayAfterInstall(const QString& topLevelName);
    void installResultsSequential(const QVariantList& results,
                                  const QString& topLevelName,
                                  int index,
                                  QStringList failures = QStringList{});
    void installOnePackage(const QVariantMap& downloadResult,
                           std::function<void(bool, const QString&)> onDone);

    // Drive the in-flight registry. setOpStage updates the InstallRegistry entry
    // and emits catalogInstallStageChanged.
    void setOpStage(const QString& name, InstallStage::Value stage);

    // Wiring (not owned — see ctor comment).
    LogosAPI*          m_logosAPI;
    CoreModuleManager* m_coreModuleManager;
    UIPluginManager*   m_uiPluginManager;
    AppsModel*         m_appsModel;
    AppsFilterProxy*   m_requiredPackagesModel = nullptr;

    // Package-state caches sourced from the package_manager module.
    QMap<QString, QString>     m_installTypeByModule;
    QMap<QString, QString>     m_displayNameByModule;
    QMap<QString, QStringList> m_missingDepsByModule;
    QMap<QString, QStringList> m_dependentsByModule;

    PendingAction m_pendingAction;
    QString m_pendingInstallPath;
    QHash<QString, QVariantList> m_versionsByRepoAndName;
    static QString catalogKey(const QString& repositoryUrl, const QString& name)
        { return repositoryUrl + QLatin1Char('\n') + name; }

    QHash<QString, QString> m_repoByName;
    QVariantList m_installedPackagesCache;
    QSet<QString>            m_installedNameSet;
    QHash<QString, QString>  m_installedVersionByName;
    QHash<QString, QString>  m_installedHashByName;   // name → rootHash of
                                                     // what's on disk. Used
                                                     // by populateAppsModel
                                                     // to feed AppsModel's
                                                     // DifferentHash detection.
    QHash<QString, int> m_dialogResolveEpoch;
    QString m_activeAddDialogName;

    // Last resolver output per top-level: raw IPC rows and derived changes.
    QHash<QString, QVariantList> m_lastResolvedRawByName;
    QHash<QString, QVariantList> m_lastResolvedChangesByName;

    InstallRegistry* m_installRegistry = nullptr;

    QVariantList m_repositories;
    int          m_repositoriesLoadingCount = 0;
    bool         m_appsLoading              = true;
};
