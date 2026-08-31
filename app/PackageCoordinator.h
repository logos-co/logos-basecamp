#pragma once

#include "InstallEnums.h"
#include "UninstallPlan.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <QMap>
#include <QSet>
#include "logos_api.h"

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
//   * Install confirmation — the requestInstall gate raised by
//     package_manager_ui (beforeInstall → dialog → confirm/cancelInstallGate).
//     Basecamp never initiates an install itself; PMU owns that, including
//     local .lgx picks.
//   * Gated uninstall/upgrade flow — requestUninstall / ackPendingAction /
//     confirmUninstall / cancelUninstall (and the upgrade siblings), plus the
//     cascade-unload + confirm handshake driven by beforeUninstall /
//     beforeUpgrade events.
//   * Pending-action state for the package-lifecycle cascade (one slot for
//     UninstallCascade / UpgradeCascade / MultiUninstallCascade).
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



    // Read-only accessors over the package-state caches. Empty when the
    // async refresh chain hasn't completed yet; QML and UIPluginManager
    // are expected to treat "empty" as "not known — show safe defaults".
    QString     installType(const QString& name) const;

    // The Merkle root over the CONTENTS ON DISK, as recorded at install. Not
    // the catalog's claim about the artifact — what is actually installed.
    QString     installedRootHash(const QString& name) const;
    QStringList missingDepsOf(const QString& name) const;
    // The same set as missingDepsOf with the reason attached: one map per
    // blocking dependency, {name, kind, requiredVersion, installedVersion,
    // requiredSigner, signerDid, detail} from logos::dependencyBlockerToMap.
    // The two are populated together and always agree on membership; a caller
    // that has to TELL the user something wants this one.
    QVariantList blockingDepsOf(const QString& name) const;
    QStringList dependentsOf(const QString& name) const;
    QString     displayNameFor(const QString& name) const;

    // Last-known package_downloader repository list, refreshed on demand via
    // refreshRepositories() and after every successful add/remove/toggle.
    QVariantList repositories() const { return m_repositories; }
    bool repositoriesLoading() const { return m_repositoriesLoadingCount > 0; }

    // True during the initial catalog populate and during a user-initiated
    // App Manager Reload (remoteRefresh). Background refreshes (file-install
    // events, etc.) leave this false so they don't flash the overlay.
    bool appsLoading() const { return m_appsLoading; }

    // False until refreshDependencyInfo has completed at least once.
    // uninstallApp reads this to decide fast-path vs loading-then-defer;
    // no other caller gates on it.
    bool dependencyDataReady() const { return m_dependencyDataReady; }

public slots:
    // Install gate (package_manager_ui-initiated). The module holds the pending
    // install; these just forward the user's decision back so it either emits
    // installApproved (PMU then installs — downloading first for a catalog
    // package, or using the local .lgx path it stashed) or installCancelled.
    // Basecamp owns no install flow of its own: every install in the app is
    // initiated by package_manager_ui and confirmed through this gate.
    Q_INVOKABLE void confirmInstallGate(const QString& name);
    Q_INVOKABLE void cancelInstallGate(const QString& name);

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
    Q_INVOKABLE void uninstallApp(const QString& name,
                                  const QString& repositoryUrl = QString());

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

    // Drops the local "waiting for dep data" state used by uninstallApp
    // before any module IPC has fired. No-op if the name doesn't match.
    Q_INVOKABLE void cancelPendingUninstallApp(const QString& name);

    // Full rescan of the package catalog. Fires the same fetchUiPluginMetadata
    // → refreshDependencyInfo chain the file-install event subscriptions
    // trigger — used by the UI Modules tab's Reload button (forwarded from
    // UIPluginManager::refreshUiModules) and by MainUIBackend right after
    // construction to do the first-time catalog load once all three managers
    // are wired. Kept as a public slot rather than running from the ctor so
    // the initial uiPluginsFetched signal isn't emitted before listeners have
    // had a chance to connect.
    Q_INVOKABLE void refresh();

    // User-initiated "Reload apps" from the App Manager. Forces the
    // downloader to re-fetch every enabled repo's logos-repo.json
    Q_INVOKABLE void remoteRefresh();

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
    void uninstallPlanRequested(const QVariantMap& plan);

    // The resolver's required-package entries, in install order. Published
    // rather than written: this used to call setRequiredPackages() on an
    // AppsFilterProxy the host held a pointer to, which is a host reaching
    // across into a shell-owned object. QML binds to it now.
    void requiredPackagesResolved(const QVariantList& entries);
    void dependencyDataReadyChanged();

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
                                             const QStringList& loadedDependents,
                                             const QVariantList& depChanges);

    // Fresh-install confirmation dialog trigger — the sibling of the upgrade
    // cascade for a not-yet-installed package coming through the module's
    // requestInstall gate, initiated by package_manager_ui. Carries the target
    // version and the resolved transitive `depChanges` so the single basecamp
    // dialog lists exactly what else will be installed. No dependents list —
    // a fresh install removes/unloads nothing. `depChanges` is empty when the
    // initiator had no catalog to resolve against (a local .lgx).
    void installGateConfirmationRequested(const QString& name,
                                          const QString& releaseTag,
                                          const QVariantList& depChanges);

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
    // signal: uninstallPlanRequested for a real removal (as a batch of one),
    // upgradeCascadeConfirmationRequested for a version swap (so the
    // dialog can lead with the target version + UpgradeMode instead of
    // bare "Uninstall and Unload Dependents?"). An ack rejection means
    // the module already cancelled (timer fired or racing listener), so
    // we stay silent rather than showing a dead dialog.
    void onBeforeUninstall(const QString& name, const QStringList& installedDeps);
    void onBeforeUpgrade(const QString& name, const QString& releaseTag,
                         int mode, const QStringList& installedDeps,
                         const QVariantList& depChanges);

    // beforeInstall handler — the catalog-install counterpart of onBeforeUpgrade.
    // Acks synchronously (cancelling the module's ack timer), then emits
    // installGateConfirmationRequested so the shared dialog can confirm the
    // install and list its transitive dep changes. No pending-slot / cascade
    // work: a fresh install unloads nothing, so confirm/cancel just forward the
    // decision to the module via confirmInstallGate / cancelInstallGate.
    void onBeforeInstall(const QString& name, const QString& releaseTag,
                         const QVariantList& depChanges);

    // Multi-uninstall variant — same ack-then-emit-dialog shape, but holds the
    // batch's full name list in m_pendingAction.names so confirm/cancel can
    // forward the same list back to the module.
    void onBeforeMultiUninstall(const QStringList& names, const QStringList& installedDeps);

private:
    // The gated-cascade pending slot. UnloadCascade (local, no IPC) lives on
    // UIPluginManager. Here we only track the ops that the package_manager
    // module itself gates (uninstall / upgrade / multi-uninstall).
    enum class PendingOp { None, UninstallCascade, UpgradeCascade, MultiUninstallCascade };
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

    // Assemble the plan input from the package-state caches. Compose vs
    // explain is picked by the caller via uninstallplan::composeFrom /
    // ::explainOf — this just wires the caches into `Input`.
    uninstallplan::Input planInput(const QStringList& targets) const;

    // The QVariantMap the uninstall dialog renders:
    //
    //   {
    //     kind:       "app" | "packages",
    //     multi:      bool,                    // confirm route: multi vs single
    //     batch:      ["chat_ui", "chat_module", ...],
    //     removable:  [{ name, displayName, version, isTarget, isLoaded }],
    //     kept:       [{ name, displayName, reason, requiredBy: [] }],
    //     dependents: [{ name, displayName, isLoaded }],
    //   }
    //
    // One map rather than positional arguments — the signals it replaced
    // already carried five and six.
    //
    // `installedDependents` comes from the module's gate event, which is the
    // authoritative view of what breaks; the locally-computed equivalent is
    // only used when the module didn't supply one.
    QVariantMap buildPlanPayload(const QStringList& batch,
                                 const QStringList& installedDependents,
                                 const QString& kind,
                                 bool multi) const;

    // Stub payload for the pre-cache-ready UninstallDialog spinner. The
    // real plan follows in a second payload once the module acks.
    QVariantMap buildLoadingPayload(const QString& name,
                                    const QString& kind) const;

    // Cache-ready half of uninstallApp: compose batch → requestMultiUninstall.
    void performUninstallApp(const QString& name);

    // Everything loaded right now — core modules from liblogos plus UI plugin
    // widgets mounted in this process. Feeds the plan's isLoaded flags.
    QSet<QString> loadedNames() const;

    // Full rescan: getInstalledPackages → per-entry resolveFlatDependencies +
    // resolveFlatDependents. Populates m_installTypeByModule,
    // m_missingDepsByModule, m_dependenciesByModule, m_dependentsByModule.
    // Emits uiModulesChanged +
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
    // Unload/tear down a package and its loaded dependents ahead of any
    // destructive package-lifecycle work. Shared by confirmUninstallCascade
    // (the gated uninstall/upgrade flow) and by the App Manager's
    // replace-before-install step, so both run identical teardown.
    void cascadeUnloadForPackage(const QString& moduleName);

    // installOnePackage tears down + removes an already-installed package
    // before handing off to installDownloadedFile, the bare installPlugin call.
    void installOnePackage(const QVariantMap& downloadResult,
                           std::function<void(bool, const QString&)> onDone);
    void installDownloadedFile(const QVariantMap& downloadResult,
                               std::function<void(bool, const QString&)> onDone);

    // Drive the in-flight registry. setOpStage updates the InstallRegistry entry
    // and emits catalogInstallStageChanged.
    void setOpStage(const QString& name, InstallStage::Value stage);

    // Wiring (not owned — see ctor comment).
    LogosAPI*          m_logosAPI;
    CoreModuleManager* m_coreModuleManager;

    // Guards for the deferred event subscriptions below. A module that is not
    // loaded at construction may be loaded later in the session, so the
    // subscriptions are retried on coreModulesChanged(); these keep that
    // idempotent, and keep the "not loaded" warning to one line rather than one
    // per stats-timer tick.
    bool m_packageManagerSubscribed = false;
    bool m_packageDownloaderSubscribed = false;
    bool m_warnedPackageManagerMissing = false;
    bool m_warnedPackageDownloaderMissing = false;
    UIPluginManager*   m_uiPluginManager;
    AppsModel*         m_appsModel;

    // Package-state caches sourced from the package_manager module.
    QMap<QString, QString>     m_installTypeByModule;
    QMap<QString, QString>     m_displayNameByModule;
    QMap<QString, QStringList> m_missingDepsByModule;
    // Same membership as m_missingDepsByModule, carrying WHY each entry
    // blocks. Filled by the same pass; see blockingDepsOf.
    QMap<QString, QVariantList> m_blockingDepsByModule;
    QMap<QString, QStringList> m_dependentsByModule;
    // Full recursive forward closure per installed package. Same
    // resolveFlatDependencies call that already feeds m_missingDepsByModule —
    // that one keeps only the rows that BLOCK a load, this one keeps the
    // rest, so the whole on-disk graph is available host-side with no extra
    // IPC.
    QMap<QString, QStringList> m_dependenciesByModule;
    bool m_dependencyDataReady = false;

    // The names the user actually asked to remove, remembered across the
    // request so the popup can tell "the app" apart from "what it dragged
    // in". Cleared once the payload is built; empty means every row in the
    // batch is an explicit target, which is the correct reading for a
    // package_manager_ui or Settings-initiated uninstall.
    QStringList m_lastRequestedTargets;
    QString     m_lastRequestKind = QStringLiteral("packages");

    // App-Manager target parked on dependencyDataReadyChanged; empty when idle.
    QString m_pendingUninstallAppName;
    QMetaObject::Connection m_pendingUninstallAppConn;

    PendingAction m_pendingAction;
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
