#pragma once

#include "InstallEnums.h"
#include "UninstallPlan.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <QMap>
#include <QSet>

#include <functional>
#include <utility>

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
//   * Servicing the three `logos.packages.confirm_*` intents raised by
//     package_manager_ui: draw the dialog, run the cascade-unload, answer. The
//     answer IS the permission — PMU removes nothing until it arrives, so the
//     unload has always finished first. Basecamp initiates no install of its
//     own here; PMU owns that, including local .lgx picks.
//   * The shell's own uninstall dialogs (Settings → Modules / Apps), which
//     carry no intent and so perform the removal themselves.
//   * Pending-action state for the cascade (one slot).
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

    // How a decision reaches the requester. Set by MainUIBackend, which owns
    // the broker — keeping it a callback is what stops this class needing one.
    // Returns whether the broker accepted the response; false means the
    // dispatch already ended (requester gone, or timed out) and nobody heard.
    using IntentResponder = std::function<bool(const QString& requestId, bool ok,
                                               const QString& error)>;
    void setIntentResponder(IntentResponder responder)
    { m_intentResponder = std::move(responder); }

    // Entry point for the three `logos.packages.confirm_*` intents. Opens the
    // matching dialog and takes ownership of answering `dispatchId`. Returns
    // false when the request cannot be shown at all, so the broker can fail it
    // closed rather than park it behind a dialog that never appeared.
    bool beginPackageConfirmation(const QString& dispatchId,
                                  const QString& intent,
                                  const QVariantMap& params,
                                  const QString& requesterName);

public slots:
    // Install gate (package_manager_ui-initiated). Answer the pending
    // confirm_install intent; PMU then runs the install itself. Basecamp owns
    // no install flow of its own here — every install in the app is initiated
    // by package_manager_ui and confirmed through this gate.
    Q_INVOKABLE void confirmInstallGate(const QString& name);
    Q_INVOKABLE void cancelInstallGate(const QString& name);

    Q_INVOKABLE void openApp(const QString& name,
                             const QString& repositoryUrl,
                             const QVariantMap& versionPins = QVariantMap(),
                             bool allowFastLaunch = true);
    Q_INVOKABLE void confirmCatalogInstall(const QString& name,
                                           const QString& repositoryUrl,
                                           const QVariantMap& versionPins = QVariantMap());

    // Shell-initiated uninstall (Settings → Modules / Apps). These raise the
    // confirm dialog directly — the shell is both asker and decider here, so
    // there is no intent. uninstallUiModule additionally refuses "main_ui"
    // because uninstalling it would brick Basecamp.
    Q_INVOKABLE void uninstallUiModule(const QString& moduleName);
    Q_INVOKABLE void uninstallCoreModule(const QString& moduleName);
    Q_INVOKABLE void uninstallApp(const QString& name,
                                  const QString& repositoryUrl = QString());

    // Cascade confirmation — called from QML once the user OKs the uninstall
    // or upgrade dialog. Unloads, then answers the intent recorded on the
    // pending action (or removes locally when there is none).
    Q_INVOKABLE void confirmUninstallCascade(const QString& moduleName);

    // Multi-uninstall counterparts. confirm runs the cascade-unload for every
    // name in the batch, then answers the requester; cancel just answers.
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
    // `requesterName` is host-attested — who asked, for the dialog to name.
    void upgradeCascadeConfirmationRequested(const QString& name,
                                             const QString& releaseTag,
                                             int mode,
                                             const QStringList& installedDependents,
                                             const QStringList& loadedDependents,
                                             const QVariantList& depChanges,
                                             const QString& requesterName,
                                             bool requesterBundled);

    // Fresh-install confirmation dialog trigger — the sibling of the upgrade
    // cascade for a not-yet-installed package. Carries the target version and
    // the resolved transitive `depChanges` so the dialog lists exactly what
    // else will be installed. No dependents list — a fresh install unloads
    // nothing. `depChanges` is empty for a local .lgx (no catalog to resolve).
    // Unlike the removal intents this one is open to any app, so naming the
    // requester is the only thing telling the user who asked.
    void installGateConfirmationRequested(const QString& name,
                                          const QString& releaseTag,
                                          const QVariantList& depChanges,
                                          const QString& requesterName,
                                          bool requesterBundled);

    // Repository management — change-notify for the QML-facing cache and
    // an outcome signal for add/remove/toggle (success or error string).
    void repositoriesChanged();
    void repositoriesLoadingChanged();
    void appsLoadingChanged();
    void repositoryOperationCompleted(const QString& operation,
                                      const QString& url,
                                      bool success,
                                      const QString& error);

private:
    // The confirm_install intent on screen, and the package it names. Install
    // has no cascade and so no PendingAction to hang the id off; uninstall and
    // upgrade carry theirs in PendingAction::intentRequestId instead.
    //
    // The id is ALWAYS paired with its subject and answered only via
    // finishIntent(id, …). Answering "whatever is pending" would let a click on
    // one dialog approve a different request — the shell's own uninstall dialog
    // takes no id at all, so it would otherwise silently approve an app's
    // in-flight install.
    QString m_pendingInstallRequestId;
    QString m_pendingInstallName;

    // Answer `requestId` and report whether the broker accepted it. A false
    // return means the requester is gone or the dispatch already ended, which
    // is the signal to finish the job locally rather than assume PMU will.
    // Empty id -> false, no call: that is how a shell-initiated flow is told it
    // owns the removal.
    bool finishIntent(const QString& requestId, bool ok,
                      const QString& error = QString());

    IntentResponder m_intentResponder;

    // Open the confirm dialog for a shell-initiated removal (Settings →
    // Modules / Apps). No intent: we asked ourselves, so we also do the work.
    void beginLocalUninstall(const QStringList& names, const QString& kind);

    // Remove each name. Only after the cascade-unload has run.
    void performLocalRemoval(const QStringList& names);

    // True when the requester is an embedded (bundled) package.
    bool requesterIsBundled(const QString& requesterName) const;

    // Union of the batch's installed dependents, excluding the batch itself.
    QStringList dependentsOfBatch(const QStringList& names) const;

    // Resolve what ELSE changes if (name, version) from `repositoryUrl` is
    // installed, then hand the list to `then`.
    //
    // WE resolve it; the requester does not send it. A caller-supplied change
    // list would let any app script the shell's own confirmation dialog, and
    // confirm_install is open to every app.
    //
    // `then` runs exactly once. On a resolver failure — or with no repository
    // to resolve against, which is every local .lgx — it runs with an empty
    // list rather than being dropped: the dialog degrades to "nothing else
    // needs to change", which is the honest reading, and the pending intent is
    // never stranded waiting for a callback that isn't coming.
    void resolveDepChangesThen(const QString& name,
                               const QString& repositoryUrl,
                               const QString& version,
                               std::function<void(const QVariantList&)> then);

    // The cascade pending slot. UnloadCascade (local, no IPC) lives on
    // UIPluginManager. Here we track what the confirm dialog is deciding.
    // Uninstall and upgrade behave identically on this side — both unload,
    // then hand back — so one op covers them; what differs is only what the
    // requester does next, which is PMU's business.
    enum class PendingOp { None, UninstallCascade, MultiUninstallCascade };
    struct PendingAction {
        PendingOp   op = PendingOp::None;
        QString     name;              // UninstallCascade
        QStringList names;             // MultiUninstallCascade — full batch
        // Empty for a shell-initiated dialog, which is exactly what tells the
        // confirm path the removal is ours rather than the requester's.
        QString     intentRequestId;
        // Upgrade and uninstall unload identically, but they must NOT fall back
        // identically: if the requester never hears the answer, removing is the
        // right recovery for an uninstall and a disaster for an upgrade, which
        // would delete the package with nothing left to install the new version.
        bool        isUpgrade = false;
    };

    // Subscribe to corePluginFileInstalled/uiPluginFileInstalled/
    // corePluginUninstalled/uiPluginUninstalled, and configure the module's
    // install directories.
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
                                 bool multi,
                                 const QString& requesterName = QString()) const;

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

    // The names the user actually asked to remove, so the popup can tell "the
    // app" apart from "what it dragged in". Cleared once the payload is built;
    // empty means every row in the batch is an explicit target, which is the
    // correct reading everywhere except the App Manager.
    QStringList m_lastRequestedTargets;

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
