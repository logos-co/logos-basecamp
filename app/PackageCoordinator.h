#pragma once

#include "ResolverRequest.h"
#include <memory>

namespace logos { class OptionalDependencyPreview; }
namespace logos::trust { struct Package; class PublisherKeyring; }

#include "InstallEnums.h"
#include "utils/HostProfile.h"
#include "utils/FreshReads.h"
#include "utils/PlanBasis.h"
#include "UninstallPlan.h"

#include <QDateTime>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <QHash>
#include <QMap>
#include <QSet>

#include <functional>
#include <utility>

#include "logos_api.h"

class AppsModel;
class CoreModuleManager;
class UIPluginManager;

// PackageCoordinator — owns every interaction with the `package_manager` LogosAPI
// module.
//
// Scope:
//   * Package scanning (getInstalledPackagesAsync, getInstalledUiPluginsAsync)
//     and the derived installType / missing-deps / dependents caches.
//   * Servicing the three `basecamp.packages.confirm_*` intents raised by the
//     installer apps (App Manager, Package Manager): draw the dialog, run the
//     cascade-unload, answer. The answer IS the permission — the requester
//     removes nothing until it arrives, so the unload has always finished
//     first. Basecamp installs nothing itself; the installer apps do, Package
//     Manager's local .lgx picks included.
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
                            const LogosBasecamp::HostProfile& hostProfile = {},
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

    // package_downloader's download source: "any", "logos" or "http". Empty
    // until it answers, and when it predates the setting.
    QString downloadSource() const { return m_downloadSource; }

    // First-run onboarding is ahead: nothing starts the downloader, and with it
    // the storage node, until chooseOnboardingSource() has chosen the source.
    bool onboardingRequired() const { return m_onboardingRequired; }

    // package_downloader's use of the storage node: "off", "waiting", "idle"
    // (stopped; the next download starts it), "starting", "running" or
    // "failed" (storageError says why). Empty until
    // it answers, and when it predates the setting.
    QString storageState() const { return m_storageState; }
    QString storageError() const { return m_storageError; }
    QVariantMap repositorySource(const QString& repositoryUrl) const;

    // True during the first catalog fetch (see requestCatalog). Background
    // refreshes (file-install events, etc.) leave this false so they don't
    // flash the overlay.
    bool appsLoading() const { return m_appsLoading; }

    // False until refreshDependencyInfo has completed at least once. An
    // uninstall confirmation waits for it: its plan is read off that data.
    bool dependencyDataReady() const { return m_dependencyDataReady; }

    // How a decision reaches the requester. Set by MainUIBackend, which owns
    // the broker — keeping it a callback is what stops this class needing one.
    // `heard` learns whether the broker took the response; false means the
    // dispatch already ended (requester gone, or timed out) and nobody heard.
    using IntentResponder = std::function<void(const QString& requestId, bool ok, const QString& error,
                                               const QVariant& data, std::function<void(bool heard)> heard)>;
    void setIntentResponder(IntentResponder responder)
    { m_intentResponder = std::move(responder); }
    // Whether an approved change is being made now (ConfirmationQueue): one
    // approved meanwhile is approved for later.
    void setChangeInFlight(std::function<bool()> inFlight) { m_changeInFlight = std::move(inFlight); }

    // Entry point for the three `basecamp.packages.confirm_*` intents. Opens the
    // matching dialog, or answers at once when it can't (another is open, the
    // request is malformed), and owns answering `dispatchId` either way.
    void beginPackageConfirmation(const QString& dispatchId,
                                  const QString& intent,
                                  const QVariantMap& params,
                                  const QString& requesterName);

public slots:
    // Install gate (package_manager_ui-initiated). Answer the pending
    // confirm_install intent; PMU then runs the install itself. Basecamp owns
    // no install flow of its own here — every install in the app is initiated
    // by package_manager_ui and confirmed through this gate.
    // `trustPublishers` (here and in confirmUninstallCascade) is "Trust &
    // install": the plan's publishers not trusted yet are
    // trusted first. A plan holding anything invalid is refused either way.
    Q_INVOKABLE void confirmInstallGate(const QString& name, const QStringList& optionalNames = {}, const QVariantMap& optionalVersionPins = {}, bool trustPublishers = false);
    Q_INVOKABLE void cancelInstallGate(const QString& name);
    Q_INVOKABLE void refreshOptionalPreview(const QString& name,
                                           const QVariantMap& optionalSelection,
                                           const QVariantMap& optionalVersionPins);

    // Launches `name` when it is installed (and `allowFastLaunch`); otherwise
    // asks for the installer app of it, from `repositoryUrl` (installRequested).
    Q_INVOKABLE void openApp(const QString& name,
                             const QString& repositoryUrl,
                             bool allowFastLaunch = true);

    // Cascade confirmation — called from QML once the user OKs the uninstall
    // or upgrade dialog. Unloads, then answers the intent recorded on the
    // pending action.
    Q_INVOKABLE void confirmUninstallCascade(const QString& moduleName, const QStringList& optionalNames = {}, const QVariantMap& optionalVersionPins = {}, bool trustPublishers = false);

    // Settings → Trusted Publishers. Deleting one, Logos too, changes future
    // installs only: what is installed keeps working.
    Q_INVOKABLE void refreshTrustedPublishers();
    Q_INVOKABLE void removeTrustedPublisher(const QString& keyName);
    Q_INVOKABLE QStringList installedFromPublisher(const QString& did) const;

    // Multi-uninstall counterparts. confirm runs the cascade-unload for every
    // name in the batch, then answers the requester; cancel just answers.
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
    // construction to do the first-time scan once all three managers are
    // wired. The catalog follows only once something has asked for it (see
    // requestCatalog). Kept as a public slot rather than running from the ctor so
    // the initial uiPluginsFetched signal isn't emitted before listeners have
    // had a chance to connect.
    Q_INVOKABLE void refresh();

    // The catalog is fetched the first time something needs it, not at launch:
    // fetching it starts package_downloader, and with it the storage node.
    // requestCatalog() fetches it (and the repository list) if nothing has yet;
    // every refresh after that fetches them again. whenCatalogLoaded() runs
    // `then` once a fetch has finished, at once if one already has, whether or
    // not it found anything. `then` runs only while this object is alive.
    Q_INVOKABLE void requestCatalog();
    void whenCatalogLoaded(std::function<void()> then);
    bool catalogFetched() const { return m_catalogFetched; }
    // PublisherKeyring::publishers().
    QVariantList trustedPublishers() const;
    // Who signed the installed copy of `name`, as summary()'s byName says it
    // ({status, publisher, impersonates}): unknown until the installed
    // packages and the keyring are loaded.
    QVariantMap installedTrust(const QString& name) const;
    // True when the requester is an embedded (bundled) package.
    bool requesterIsBundled(const QString& requesterName) const;
    // A confirmation request the shell was answering ended without that
    // answer: its dialog closes, and the next confirmation isn't refused.
    void shellIntentEnded(const QString& dispatchId);

    // Package-repository management — thin wrappers around the
    // package_downloader IPC surface
    Q_INVOKABLE void refreshRepositories();
    Q_INVOKABLE void addRepository(const QString& url);
    Q_INVOKABLE void removeRepository(const QString& url);
    Q_INVOKABLE void setRepositoryEnabled(const QString& url, bool enabled);
    Q_INVOKABLE void refreshDownloadSource();
    Q_INVOKABLE void setDownloadSource(const QString& source);

    // Starts the downloader with `source`; called again, changes the source.
    Q_INVOKABLE void chooseOnboardingSource(const QString& source);
    // Records onboarding as done.
    Q_INVOKABLE void completeOnboarding();

    // Reloads storage_module and restarts the downloader, which starts the
    // node again once it is ready. Cancels the downloads in flight.
    Q_INVOKABLE void retryStorage();

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

    void launchAppRequested(const QString& name);
    // Not installed, or asked for its details: its installer app shows it.
    void installRequested(const QString& name, const QString& repositoryUrl);
    // package_manager's installed packages, once a refresh has read them and
    // their dependencies, and every cache holds them.
    void installedPackagesRead(const QVariantList& packages);
    // A package's download moving, or done (package_downloader's events).
    void packageOnItsWay(const QString& name);
    // A package's files installed or removed (package_manager's events).
    void packageFilesChanged();
    void uninstallPlanRequested(const QVariantMap& plan);
    void dependencyDataReadyChanged();

    // Upgrade/Downgrade/Reinstall cascade dialog trigger. Same dependent-
    // impact lists as the uninstall variant (the package_manager performs
    // an uninstall step first), but carries the target version + the
    // UpgradeMode so the dialog can lead with "Upgrade to v1.2.3" /
    // "Downgrade to v1.0.0" / "Reinstall v1.0.0" instead of bare
    // "Uninstall and Unload Dependents?". `mode` mirrors
    // PackageTypes::UpgradeMode (0=Upgrade, 1=Downgrade, 2=Sidegrade).
    // `requesterName` is host-attested — who asked, for the dialog to name.
    // `requestId`: the request it answers, empty for Basecamp's own.
    void upgradeCascadeConfirmationRequested(const QString& name,
                                             const QString& releaseTag,
                                             int mode,
                                             const QStringList& installedDependents,
                                             const QStringList& loadedDependents,
                                             const QVariantList& depChanges,
                                             const QString& requesterName,
                                             bool requesterBundled,
                                             const QString& requestId);

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
                                          bool requesterBundled,
                                          bool depChangesResolved,
                                          const QString& requestId);
    void optionalGatePreviewUpdated(const QString& name, const QVariantList& changes, bool pending);
    // The request `requestId` a confirmation was shown for ended unanswered:
    // its dialog closes, and answers nothing.
    void confirmationWithdrawn(const QString& requestId);

    // Repository management — change-notify for the QML-facing cache and
    // an outcome signal for add/remove/toggle (success or error string).
    void repositoriesChanged();
    void repositoriesLoadingChanged();
    void downloadSourceChanged();
    void onboardingRequiredChanged();
    void storageStateChanged();
    void appsLoadingChanged();
    void repositoryOperationCompleted(const QString& operation,
                                      const QString& url,
                                      bool success,
                                      const QString& error);

    // Trust at install (utils/InstallTrust.h): what the install or upgrade
    // dialog shows about who signed its plan -- {pending: true} while that is
    // checked; and the trusted publishers changing.
    void installTrustUpdated(const QString& name, const QVariantMap& trust);
    void trustedPublishersChanged();
    void trustedPublisherChangeFailed(const QString& keyName, const QString& message);
    // A confirmed package_manager_ui install or upgrade that did not go ahead.
    void installRefused(const QString& name, const QString& reason);
    // A copy of one of Basecamp's own apps listed from the user's plugins dir
    // (utils/BuiltInApps.h), now in `to`, or left in `from` when `to` is
    // empty. `stopped` is always false: a listed app is not a running module.
    void builtInCopySetAside(const QString& name, const QString& from, const QString& to, bool stopped);

private:
    // The confirm_install intent on screen, and the package it names. Install
    // has no cascade and so no PendingAction to hang the id off; uninstall and
    // upgrade carry theirs in PendingAction::intentRequestId instead.
    //
    const LogosBasecamp::HostProfile m_hostProfile;

    // The id is ALWAYS paired with its subject and answered only via
    // finishIntent(id, …). Answering "whatever is pending" would let a click on
    // one dialog approve a different request.
    QString m_pendingInstallRequestId;
    QString m_pendingInstallName;

    // Answer `requestId`; `heard` learns whether the broker accepted it. False
    // means the requester is gone or the dispatch already ended, which is the
    // signal to finish the job locally rather than assume PMU will. An empty
    // id is nobody's: false, no call.
    void finishIntent(const QString& requestId, bool ok, const QString& error = QString(),
                      const QVariant& data = QVariant(), std::function<void(bool heard)> heard = {});

    QVariantList m_pendingOptionalPackages;
    // The reads of the installed packages (refreshDependencyInfo()), newest
    // first, and the confirmations waiting for one.
    logos::FreshReads m_freshReads;
    QString m_pendingPreviewName, m_pendingPreviewRepo;
    bool m_pendingPreviewNewest = false;
    // What the requester picked: its versions stay fixed while the person
    // changes the optional packages in the dialog.
    logos::InstallChoices m_pendingPreviewChoices;
    int m_gatePreviewEpoch = 0;
    QVariantMap selectedOptionalPackages(const QStringList& names, const QVariantMap& versionPins) const;
    void endGatePreview(const QString& name);
    QVariantMap optionalPackageRow(const QVariantMap& offer) const;
    QVariantList gatePreviewChanges(const QVariantList& resolved);
    // `rows` with `closes` on each that replaces an installed release: what
    // replacing it stops (utils/ReplaceImpact.h).
    QVariantList withReplaceImpact(const QVariantList& rows) const;
    // Unloads what an approved install or upgrade of `name` replaces, and
    // what runs on it, before the answer lets the installer swap the files.
    void unloadReplaced(const QString& name, const QVariantMap& answer);
    QVariantList optionalRowsWithChildren(const QVariantList& offerRows, const QVariantList& changes) const;

    IntentResponder m_intentResponder;

    // Remove each name. Only after the cascade-unload has run.
    void performLocalRemoval(const QStringList& names);

    // Union of the batch's installed dependents, excluding the batch itself.
    QStringList dependentsOfBatch(const QStringList& names) const;

    // Resolve what ELSE changes if `name` from `repositoryUrl` is installed,
    // at the versions `choices` pins (its own among them), then hand the list
    // to `then`.
    //
    // WE resolve it; the requester does not send it. A caller-supplied change
    // list would let any app script the shell's own confirmation dialog, and
    // confirm_install is open to every app.
    //
    // `then` runs exactly once. On a resolver failure — or with no repository
    // to resolve against, which is every local .lgx — it runs with an empty
    // list rather than being dropped, so the pending intent is never stranded
    // waiting for a callback that isn't coming.
    //
    // `then(resolved, changes)`. `resolved` says whether the resolver actually
    // ran: an empty `changes` means "nothing needs to change" ONLY when it is
    // true. For a local file (nothing to resolve against), or a failed call,
    // the gate has established nothing — and a consent dialog must not report
    // an absence of knowledge as an absence of consequences. An empty
    // `repositoryUrl` lets the resolver pick the repository.
    //
    // `newestReleases`: resolve every package to its newest catalog release,
    // as App Manager installs, rather than keeping what is installed where it
    // satisfies the range, as Package Manager's rows (each only itself) do. An
    // installed package whose contents the catalog republished then shows as
    // the Reinstall it is (basecamp#427). `choices`: logos::installChoicesFrom().
    void resolveDepChangesThen(const QString& name,
                               const QString& repositoryUrl,
                               bool localFile,
                               bool newestReleases,
                               const logos::InstallChoices& choices,
                               std::function<void(bool, const QVariantList&)> then);
    // The `apps` slot installs the newest releases (resolveDepChangesThen()).
    static bool installsNewestReleases(const QString& requesterName);

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

    // Subscribe to package_downloader's events, and start it once it is loaded.
    void subscribeToPackageDownloaderEvents();

    // package_downloader does nothing until a consumer starts it. Basecamp starts
    // it once it is loaded, and every operation that calls into it begins here,
    // which starts it again after a reload or a failed start. The operations that
    // arrive before the answer wait for it. `then` runs only while this object is
    // alive.
    void withDownloaderStarted(std::function<void()> then);
    void startDownloader();

    // The downloader starts the storage node only for a download; Basecamp
    // wants it up as soon as the source allows. Refused under "http".
    void requestStorage();

    void refreshStorageState();
    void setStorageState(const QString& state, const QString& error);

    // Under "http", unloads storage_module unless something else uses it: the
    // Storage app, or any other loaded module that depends on it.
    void releaseStorageIfUnused();

    // package_downloader's client, or null when it is not loaded in this
    // process (a host without the catalogue never loads it). isConnected() alone cannot
    // tell: it is true for a module that was never loaded.
    LogosAPIClient* downloaderClient() const;

    // Pull UI plugin metadata from the module and emit uiPluginsFetched. Also
    // seeds the installType cache for the UI-plugin subset; the full-scan pass
    // in refreshDependencyInfo overwrites it with the core-inclusive version.
    // A built-in app listed from the user's dir is set aside first and the
    // list fetched again, once (`afterSetAside`).
    void fetchUiPluginMetadata(bool afterSetAside = false);

    void tryFetchCatalog(const QHash<QString, QString>& installedByName, int retriesLeft);
    void fetchValidVariants();
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

    // Stub payload for the UninstallDialog spinner while the installed
    // packages are read. The real plan follows in a second payload.
    QVariantMap buildLoadingPayload(const QString& name,
                                    const QString& kind) const;

    // A confirm_uninstall's plan and dialog, once the installed packages are
    // known. removesAnApp(): the `apps` slot asks to remove one app, which
    // takes with it what only that app needed.
    static bool removesAnApp(const QStringList& names, const QString& requesterName);
    void planUninstall(const QString& dispatchId, const QStringList& names,
                       const QString& requesterName);

    // Everything loaded right now — core modules from liblogos plus UI plugin
    // widgets mounted in this process. Feeds the plan's isLoaded flags.
    QSet<QString> loadedNames() const;

    // Full rescan: getInstalledPackages → per-entry resolveFlatDependencies +
    // resolveFlatDependents. Populates m_installTypeByModule,
    // m_missingDepsByModule, m_dependenciesByModule, m_dependentsByModule.
    // Emits uiModulesChanged +
    // coreModulesChanged + launcherAppsChanged when done so the QML bindings
    // pick up the new installType / missing-deps values.
    // Reads the installed packages and their dependencies into the caches;
    // false when package_manager isn't there to ask.
    bool refreshDependencyInfo();
    // Runs `then(true)` once a read that started after this call is in the
    // caches (installedPackagesRead), so a confirmation is planned on what is
    // installed when it arrived, whatever let it through the queue; `then(false)`
    // when that read failed, or none can start: nothing is planned on what was
    // known before.
    void afterFreshRead(std::function<void(bool read)> then);

    QVariantList computeDepChanges(const QVariantList& resolved,
                                   const QHash<QString, QString>& installedByName) const;
    // Whether a resolved entry is already on disk: same version, same rootHash.
    static bool alreadyOnDisk(const QString& installedVersion,
                              const QString& installedHash,
                              const QString& resolvedVersion,
                              const QString& resolvedHash);

    static QString depAction(const QString& installedVersion,
                             const QString& resolvedVersion,
                             const QString& installedHash,
                             const QString& resolvedHash);
    static QVariantMap changeFromResolverEntry(const QVariantMap& entry,
                                               const QString& installedVersion,
                                               const QString& installedHash);
    QVariantMap repositoryRow(const QString& url) const;
    QString repositoryLabelFor(const QVariantMap& catalogEntry) const;

    void resolveOptionalPreview(const QString& name, const QString& repositoryUrl,
                                const QVariantMap& versionPins, const QVariantMap& selection,
                                const QVariantMap& optionalPins, const QString& installedJson,
                                std::function<bool()> current,
                                std::function<void(QVariantList)> then,
                                bool selectNew = true);
    void resolveOptionalPreviewPass(std::shared_ptr<logos::OptionalDependencyPreview> preview,
                                    const QString& request, const QString& installedJson,
                                    QSet<QString> visited, std::function<bool()> current,
                                    std::function<void(QVariantList)> then);
    // Unload/tear down a package and its loaded dependents ahead of the
    // gated uninstall or upgrade (confirmUninstallCascade).
    void cascadeUnloadForPackage(const QString& moduleName);

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

    // See withDownloaderStarted(). Only a successful start is remembered, so a
    // failed one is tried again by the next call; stateChanged keeps it in
    // step with starts and stops made by other consumers, and losing the
    // instance (an unload or reload) clears it.
    bool m_downloaderStarted = false;
    bool m_downloaderStarting = false;
    QList<std::function<void()>> m_waitingForDownloaderStart;

    bool    m_onboardingRequired = false;
    bool    m_onboardingSourceChosen = false;
    QString m_storageState;
    QString m_storageError;
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
    // correct reading everywhere except an app's removal (planUninstall()).
    QStringList m_lastRequestedTargets;

    PendingAction m_pendingAction;
    QHash<QString, QVariantList> m_versionsByRepoAndName;
    static QString catalogKey(const QString& repositoryUrl, const QString& name)
        { return repositoryUrl + QLatin1Char('\n') + name; }

    QHash<QString, QString> m_repoByName;
    QVariantList m_installedPackagesCache;
    // package_manager's record of the installed `name`, or empty.
    QVariantMap installedRecord(const QString& name) const;

    // ── Approved for later ──────────────────────────────────────────────
    // An install or upgrade whose app can hold an approval until its turn
    // (`waits`), approved while another change is being made, or shown while
    // one was, is answered {ticket}, and nothing is unloaded. Handed back at
    // its turn (`ticket`), it is answered with what was approved, on a fresh
    // read, once every package it was planned over is still installed as it
    // was and nothing new depends on one; otherwise the person is asked again.
    struct Ticket {
        QString requester;
        QString intent;
        QVariantMap request;               // name, version, repositoryUrl, mode
        QVariantMap answer;                // as approved
        logos::planbasis::Basis basis;     // releaseOf() each, as it was planned
        QDateTime expires;
    };
    QHash<QString, Ticket> m_tickets;
    std::function<bool()> m_changeInFlight;
    // The install or upgrade on screen: who asked, what, what was installed
    // and what depended on what when it was planned, and whether a change was
    // being made then.
    QString m_gateRequester;
    QString m_gateIntent;
    QVariantMap m_gateParams;
    QVariantList m_gateInstalled;
    QMap<QString, QStringList> m_gateDependents;
    bool m_gateDuringChange = false;
    void snapshotGateBasis();
    // `name`'s release in `installed` (package_manager's records), "" if none.
    static QString releaseOf(const QVariantList& installed, const QString& name);
    // A ticket for the install or upgrade on screen, approved now with
    // `answer`; "" when it is to go ahead now.
    QString approveForLater(const QString& name, const QVariantMap& answer);
    void goAhead(const QString& dispatchId, const QString& intent, const QVariantMap& params,
                 const QString& requesterName);
    // The names only Logos may install under (reservedNames()).
    QStringList m_bundledNames;
    QSet<QString>            m_installedNameSet;
    QHash<QString, QString>  m_installedVersionByName;
    QHash<QString, QString>  m_installedHashByName;   // name → rootHash of
                                                     // what's on disk. Used
                                                     // by populateAppsModel
                                                     // to feed AppsModel's
                                                     // DifferentHash detection.

    QVariantList m_repositories;
    int          m_repositoriesLoadingCount = 0;
    QString      m_downloadSource;
    bool         m_appsLoading              = false;

    // See requestCatalog(). Nothing asks for the catalog or the repository
    // list at launch; once something has, every refresh fetches them again.
    bool m_catalogWanted        = false;
    bool m_repositoriesWanted   = false;
    bool m_catalogFetched       = false;
    bool m_catalogFetchInFlight = false;
    // Bumped by each getCatalog call; an answer to an older one is dropped.
    quint64 m_catalogGeneration = 0;
    QList<std::function<void()>> m_waitingForCatalog;

    // Trust at install: the verdict on the install or upgrade on screen,
    // named by its dispatch id. A confirm acts on it only while it is about
    // what is confirmed.
    struct TrustVerdict {
        QString name;
        QString about;      // the dispatch id
        int epoch = 0;
        QVariantMap trust;  // {pending: true} while checking, then summary()
        // What confirming it holds the requester to: {plan: approvedPlan()}
        // or {file: fileIdentity()}.
        QVariantMap approval;
        // Every package it was planned over, and each app's backend.
        QStringList plannedOn;
    };
    TrustVerdict m_gateTrust;
    logos::trust::PublisherKeyring* m_publisherKeyring = nullptr;
    void configureTrust();
    // The dispatch id of the confirmation on screen for `name`, or empty.
    QString gateDispatchId(const QString& name) const;
    int  beginTrust(const QString& name);
    void publishTrust(int epoch, const QVariantMap& trust, const QVariantMap& approval = {},
                      const QStringList& plannedOn = {});
    void emitTrust();
    void assessPlanTrust(const QString& name, const QVariantList& resolved);
    void assessLocalFileTrust(const QString& name, const QString& version, const QString& path);
    // A confirm_install / confirm_upgrade's assessment, started before its
    // dialog opens.
    void beginConfirmationTrust(const QString& name, const QString& version, bool localFile,
                                const QVariantMap& params, const QString& requesterName);
    // summary() of `packages` against the keyring, for a dialog about `about`.
    QVariantMap trustSummary(const QString& about, const QList<logos::trust::Package>& packages) const;
    void withTrustedKeys(std::function<void()> then);
    // The same, once the installed packages are known (dependencyDataReady()):
    // what an install replaces is judged by the copy it replaces.
    void withInstalledAndTrustedKeys(std::function<void()> then);
    // Calls `then` with why the install may not go ahead -- the verdict is not
    // about `about` (a dispatch id) yet, holds something invalid, or a
    // publisher could not be trusted -- or with an empty string once every
    // publisher asked for is in the keyring.
    void admitTrust(const QString& name, const QString& about,
                    bool trustPublishers, std::function<void(const QString& refused)> then);
    // The answer to a package_manager_ui confirmation: the optional packages
    // chosen, and what the gate's verdict holds the install to.
    QVariantMap gateAnswer(const QString& name, const QStringList& optionalNames,
                           const QVariantMap& optionalVersionPins) const;
    // Unloads, then answers the pending uninstall or upgrade with `answer`.
    void runUninstallCascade(const QString& moduleName, const QVariantMap& answer);
    // Refuses `requestId`: what is installed could not be read for it.
    void refuseUnread(const QString& requestId, const QString& name);
    // Answers a confirmed package_manager_ui request that may not go ahead.
    void refuseConfirmed(const QString& requestId, const QString& name, const QString& reason);

    // Fetch the repository list and the catalog now, showing appsLoading.
    void fetchCatalogNow();

    // A catalog fetch ended, found something or not: run what waited on it.
    void catalogFetchFinished(bool loaded);

    // openApp's fast path: emits launchAppRequested and returns true when the
    // app's tile is installed and healthy.
    bool fastLaunch(const QString& name, const QString& repositoryUrl);
};
