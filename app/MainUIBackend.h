#pragma once

#include <functional>
#include <memory>

#include "ModuleInstanceModel.h"

#include <QAbstractItemModel>
#include "ICoreRuntime.h"
#include "utils/HostProfile.h"

#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include "logos_api.h"

class AppsModel;
class CoreModuleManager;
class PackageCoordinator;
class QWidget;
class UIPluginManager;
class IntentRegistry;
class IntentBroker;
class LinkRequestCoordinator;
class IntentBridgeAdapter;
class UIPluginPresenter;
class IntentsModuleHost;
class IntentChooserQueue;
class ConfirmationQueue;
class ShellIntentEndpoint;
class ShellIntentChooser;
class ShellIntentInstaller;



// MainUIBackend — thin QML-facing facade.
//
// Owns three managers as Qt children:
//   * CoreModuleManager — wraps the logos_core_* C API and the stats timer.
//   * UIPluginManager   — UI-plugin widget lifecycle, app launcher, local
//                         unload-cascade confirmation.
//   * PackageCoordinator    — every `package_manager` LogosAPI module interaction
//                         (install/uninstall/upgrade flows, install-confirm
//                         dialog, uninstall-cascade dialog, package-state
//                         caches).
//
// Construction order: CoreModuleManager → UIPluginManager → PackageCoordinator.
// CoreModuleManager must exist first because UIPluginManager's ctor takes it.
// UIPluginManager must exist before PackageCoordinator because PackageCoordinator's
// ctor takes it (for cascade cooperation — checking loaded state, tearing
// down UI widgets). PackageCoordinator is then injected into UIPluginManager via
// setPackageCoordinator so UIPluginManager can query installType / missing-deps
// for its uiModules() / launcherApps() builders. Qt's reverse-order child
// destruction tears PackageCoordinator → UIPluginManager → CoreModuleManager,
// which matches the data-flow dependencies (PackageCoordinator stops emitting
// before UIPluginManager tears down its widgets, which stop before the C
// API handle goes away).
//
// Everything the QML layer calls (`backend.loadUiModule(...)`, etc.) is a
// one-line delegation into one of the three managers. Signals from the
// managers that QML cares about (`launcherAppsChanged`,
// `uninstallPlanRequested`, …) are re-emitted here via signal-to-signal
// connects; the two module inspectors instead consume `uiModulesModel` /
// `coreModulesModel` and read change notifications off those model
// pointers directly. Navigation is the only behavior that lives on this
// class itself.
class MainUIBackend : public QObject {
    Q_OBJECT

    // Navigation
    Q_PROPERTY(int currentActiveSectionIndex READ currentActiveSectionIndex WRITE setCurrentActiveSectionIndex NOTIFY currentActiveSectionIndexChanged)

    // UI plugins + core modules — exposed to QML exclusively as real Qt
    // models so the Settings inspectors can bind directly through a
    // ModulesFilterProxy. The QVariantList uiModules()/coreModules() shape
    // used to be a Q_PROPERTY too; it now lives on this class only as a
    // private helper feeding the models on each *Changed tick.
    //
    // Declared as QAbstractItemModel*, not ModuleInstanceModel*. The shell's
    // QML only ever uses them as models, and typing them as the Qt base is
    // what keeps the concrete class from having to be nameable — and
    // therefore present — on the shell's side of the boundary.
    Q_PROPERTY(QAbstractItemModel* uiModulesModel   READ uiModulesModel   CONSTANT)
    Q_PROPERTY(QAbstractItemModel* coreModulesModel READ coreModulesModel CONSTANT)
    // AppsModel — catalog rows + installed state. QML views bind directly.
    // Lifetime is tied to MainUIBackend; the pointer is stable across the
    // app's life. Same reasoning as the two above: crosses as
    // QAbstractItemModel*.
    Q_PROPERTY(QAbstractItemModel* appsModel READ appsModel CONSTANT)

    // App Launcher
    Q_PROPERTY(QVariantList launcherApps READ launcherApps NOTIFY launcherAppsChanged)
    // Most-recently-closed apps, persisted across restarts.
    Q_PROPERTY(QVariantList recentlyClosedApps READ recentlyClosedApps NOTIFY recentlyClosedAppsChanged)
    Q_PROPERTY(QString currentVisibleApp READ currentVisibleApp NOTIFY currentVisibleAppChanged)
    Q_PROPERTY(QStringList loadingModules READ loadingModules NOTIFY loadingModulesChanged)
    // Open apps whose backend module stopped on its own.
    Q_PROPERTY(QStringList stoppedApps READ stoppedApps NOTIFY stoppedAppsChanged)

    // Build info (baked in at nix build time). See Dashboard view.
    //   * buildVersion: VERSION file contents (empty when not baked in).
    //   * isPortableBuild: true for distributed/release builds, false for dev.
    //   * buildCommits: list of { name, commit } for basecamp + each flake input.
    Q_PROPERTY(QString buildVersion READ buildVersion CONSTANT)
    Q_PROPERTY(bool isPortableBuild READ isPortableBuild CONSTANT)
    Q_PROPERTY(QVariantList availableSections READ availableSections CONSTANT)
    // Section index -> the default app shown as that section, where offered.
    Q_PROPERTY(QVariantMap sectionApps READ sectionApps CONSTANT)
    Q_PROPERTY(bool packageCatalog READ packageCatalog CONSTANT)
    Q_PROPERTY(bool repositoryManagement READ repositoryManagement CONSTANT)
    Q_PROPERTY(bool devHost READ devHost CONSTANT)

    //   * isMockBackend: this Basecamp is serving FIXTURE DATA. No module is
    //     running, nothing is installed or downloaded, and every list on screen
    //     came out of a JSON file.
    //
    // Surfaced in the UI deliberately. A mock-backed build is indistinguishable
    // from a working one at a glance — modules listed, catalog populated, stats
    // ticking — which is exactly what makes it dangerous to mistake for the real
    // thing. The sidebar badge is the cheapest possible guard against someone
    // filing a bug, or shipping a screenshot, from a build that was never
    // talking to anything.
    Q_PROPERTY(bool isMockBackend READ isMockBackend CONSTANT)
    Q_PROPERTY(QVariantList buildCommits READ buildCommits CONSTANT)
    // The user dir this instance uses (default or --user-dir). See Dashboard view.
    Q_PROPERTY(QString userDirectory READ userDirectory CONSTANT)

    // Package repositories
    Q_PROPERTY(QVariantList repositories READ repositories NOTIFY repositoriesChanged)
    Q_PROPERTY(QVariantList trustedPublishers READ trustedPublishers NOTIFY trustedPublishersChanged)
    Q_PROPERTY(bool repositoriesLoading READ repositoriesLoading NOTIFY repositoriesLoadingChanged)
    Q_PROPERTY(QString downloadSource READ downloadSource NOTIFY downloadSourceChanged)
    Q_PROPERTY(bool onboardingRequired READ onboardingRequired NOTIFY onboardingRequiredChanged)
    Q_PROPERTY(bool online READ online NOTIFY onlineChanged)
    Q_PROPERTY(QString storageState READ storageState NOTIFY storageStateChanged)
    Q_PROPERTY(QString storageError READ storageError NOTIFY storageStateChanged)

    // True until the first catalog fetch has finished.
    Q_PROPERTY(bool appsLoading READ appsLoading NOTIFY appsLoadingChanged)

    // Gates every Uninstall affordance until the dependency caches have been
    // populated once — see PackageCoordinator::dependencyDataReady.
    Q_PROPERTY(bool dependencyDataReady READ dependencyDataReady
               NOTIFY dependencyDataReadyChanged)

    // Settings → Modules Reload overlay. Ref-counted so UI + Core refreshes
    // kicked together (e.g. on tab show) share one spinner.
    Q_PROPERTY(bool modulesLoading READ modulesLoading NOTIFY modulesLoadingChanged)

public:
    // `intentsHost`: intents_module's presenter, already claimed; this takes it.
    explicit MainUIBackend(LogosAPI* logosAPI = nullptr,
                           ICoreRuntime* core = nullptr,
                           const LogosBasecamp::HostProfile& hostProfile = {},
                           IntentsModuleHost* intentsHost = nullptr,
                           QObject* parent = nullptr);

    // Tears down the UI-plugin layer while the shell that hosts those widgets
    // is still alive. Window calls this as step 1 of its ordered teardown; see
    // ~Window. Idempotent, and safe to skip — ~UIPluginManager still covers it.
    void beginShutdown();
    ~MainUIBackend() override;

    // Navigation — lives on this class.
    int currentActiveSectionIndex() const;

    // Delegations to UIPluginManager.
    QVariantList launcherApps() const;
    QVariantList recentlyClosedApps() const;
    QString      currentVisibleApp() const;
    QStringList  loadingModules() const;
    QStringList  stoppedApps() const;

    // Build info accessors (see Q_PROPERTY declarations above).
    QString buildVersion() const;
    bool isPortableBuild() const;
    QVariantList availableSections() const;
    QVariantMap sectionApps() const;
    bool packageCatalog() const;
    bool repositoryManagement() const;
    bool devHost() const;
    bool isMockBackend() const;
    QVariantList buildCommits() const;
    QString userDirectory() const;

    QVariantList repositories() const;
    QVariantList trustedPublishers() const;
    bool repositoriesLoading() const;
    QString downloadSource() const;
    bool onboardingRequired() const;
    bool online() const;
    QString storageState() const;
    QString storageError() const;
    bool appsLoading() const;
    bool dependencyDataReady() const;
    bool modulesLoading() const;

    // Accessors for C++ coordination code (WorkspaceArea etc.) that needs
    // a handle to the managers directly. QML goes through the delegating
    // slots/signals.
    CoreModuleManager* coreModuleManager() const { return m_coreModuleManager; }
    UIPluginManager*   uiPluginManager()   const { return m_uiPluginManager; }
    PackageCoordinator*    packageCoordinator()    const { return m_packageCoordinator; }
    // Base-typed on purpose — see the Q_PROPERTY comments above. Internal
    // code uses the concrete members directly and does not go through these.
    //
    // Defined out of line: AppsModel is only forward-declared here, so the
    // derived-to-base conversion needs the definition, and pulling AppsModel.h
    // into this header would push it into every TU that includes it.
    QAbstractItemModel* appsModel() const;
    QAbstractItemModel* uiModulesModel()   const { return m_uiModulesModel; }
    QAbstractItemModel* coreModulesModel() const { return m_coreModulesModel; }

public slots:
    // Navigation
    void setCurrentActiveSectionIndex(int index);

    // UI Module operations — delegated to UIPluginManager.
    void loadUiModule(const QString& moduleName);
    void unloadUiModule(const QString& moduleName);
    void restartUiModule(const QString& moduleName);
    void activateApp(const QString& appName);

    // Friendly module label, resolved from the catalog with fallback to `name`.
    Q_INVOKABLE QString displayNameFor(const QString& moduleName) const;
    // Shows userDirectory in the system file manager; false if it couldn't.
    Q_INVOKABLE bool openUserDirectory() const;

    // ── Intents ─────────────────────────────────────────────────────────
    //
    // Delegating one-liners, like the rest of this facade. The broker is
    // deliberately NOT exposed to the QML context: that would let any view
    // reach policy directly and break the single-surface contract.
    // Returns whether a broker took it: the QML broker answers at once,
    // intents_module only later (answerShellIntent()).
    Q_INVOKABLE bool respondToShellIntent(const QString& requestId, bool ok,
                                          const QVariant& data = QVariant(),
                                          const QString& error = QString());
    Q_INVOKABLE void resolveIntentChooser(const QString& dispatchId,
                                          const QString& providerName);
    Q_INVOKABLE void cancelIntentChooser(const QString& dispatchId);
    // The chooser dialog has closed, after its exit transition: the next one may come up.
    Q_INVOKABLE void intentChooserClosed(const QString& dispatchId);

    // Catalog packages that could service `intent` but are not installed. The
    // chooser lists these alongside the real providers so the mixed case stops
    // looking identical to "this is the only app that can do this" — which it
    // did, because nothing else in the UI surfaces what a package provides.
    Q_INVOKABLE QVariantList installableProvidersFor(const QString& intent) const;

    // Raise the install offer for one of them. The dialog answers the request
    // `cancelled` first: an uninstalled package cannot be dispatched to, and
    // holding the request open across a download would put the install's
    // duration into its lifetime.
    Q_INVOKABLE void offerInstallFor(const QString& intent,
                                     const QString& moduleName);

    // Install suggestion. The request that prompted it is already finished, so
    // there is no dispatch id and nothing to answer — just an ordinary install.
    Q_INVOKABLE void beginIntentInstall(const QString& providerName);

    // "Who is this?" from the chooser — the Package Manager's details of a
    // package (packages.show) rather than a summary in a dialog.
    Q_INVOKABLE void showPackageDetails(const QString& packageName);

    // "Install this app" from outside the apps — the welcome page's app
    // tiles, intent install offers and deep links (openApp()). Hands off to
    // App Manager (apps.install), which shows the app and asks for Basecamp's
    // gate, from `repositoryUrl` when it was picked from one. Without
    // intents_module, its section.
    Q_INVOKABLE void requestAppInstall(const QString& appName, const QString& repositoryUrl = {});
    // "Install this package", for one that isn't an app: the welcome page's
    // Packages row. Package Manager (packages.install) shows it, and the
    // person's Install there asks for the gate, as before App Manager left
    // Basecamp. Without intents_module, its section.
    Q_INVOKABLE void requestPackageInstall(const QString& packageName, const QString& repositoryUrl = {});
    // The welcome page's "show all" for apps: App Manager, searching for
    // `query`. Without intents_module, its section alone.
    Q_INVOKABLE void searchApps(const QString& query);

    // Cascade confirmation flow. Local unload cascade lives on
    // UIPluginManager; uninstall/upgrade cascade lives on PackageCoordinator.
    // cancelPendingAction fans out to both so the un-involved one no-ops.
    Q_INVOKABLE void confirmUnloadCascade(const QString& moduleName);
    Q_INVOKABLE void confirmUninstallCascade(const QString& moduleName, const QStringList& optionalNames = {}, const QVariantMap& optionalVersionPins = {}, bool trustPublishers = false);
    Q_INVOKABLE void confirmUninstallMultiCascade(const QStringList& moduleNames);
    Q_INVOKABLE void cancelMultiUninstall(const QStringList& moduleNames);
    Q_INVOKABLE void cancelPendingAction(const QString& moduleName);

    // Install gate (package_manager_ui-initiated) — delegated to
    // PackageCoordinator, which forwards the decision to the module's
    // confirmInstall / cancelInstall gate. The app's only install
    // confirmation; basecamp initiates no installs of its own.
    Q_INVOKABLE void refreshOptionalPreview(const QString& name, const QVariantMap& selection,
                                           const QVariantMap& optionalPins);
    Q_INVOKABLE void confirmInstallGate(const QString& name, const QStringList& optionalNames = {}, const QVariantMap& optionalVersionPins = {}, bool trustPublishers = false);
    Q_INVOKABLE void cancelInstallGate(const QString& name);

    // Launches an installed app, or asks App Manager for it — delegated to
    // PackageCoordinator.
    Q_INVOKABLE void openApp(const QString& name,
                             const QString& repositoryUrl,
                             bool allowFastLaunch = true);

    // Settings → Trusted Publishers (PackageCoordinator).
    Q_INVOKABLE void refreshTrustedPublishers();
    Q_INVOKABLE void removeTrustedPublisher(const QString& keyName);
    Q_INVOKABLE QStringList installedFromPublisher(const QString& did) const;

    // Core Module operations — routing rule: cascade-aware (load/unload)
    // goes through UIPluginManager so it can run the pre-flight dependent
    // check. Pure introspection (refresh, getMethods, callMethod) goes
    // directly to CoreModuleManager.
    void loadCoreModule(const QString& moduleName);
    void unloadCoreModule(const QString& moduleName);
    Q_INVOKABLE void refreshCoreModules();
    Q_INVOKABLE QString getCoreModuleMethods(const QString& moduleName);
    Q_INVOKABLE QString getCoreModuleEvents(const QString& moduleName);
    Q_INVOKABLE QString callCoreModuleMethod(const QString& moduleName, const QString& methodName, const QString& argsJson);

    // UI Modules refresh — forwarded to UIPluginManager which in turn asks
    // PackageCoordinator to rescan the catalog (PackageCoordinator owns the IPC).
    Q_INVOKABLE void refreshUiModules();

    // App Launcher operations — delegated to UIPluginManager.
    void onAppLauncherClicked(const QString& appName);
    void setCurrentVisibleApp(const QString& pluginName);

    // Pushed from OverlayDialogs.qml's anyDialogOpen binding. Read by the
    // intent presenter, which declines to auto-return while a dialog owns the
    // screen. Not a Q_PROPERTY: nothing binds to it, and a property would
    // invite QML to start driving navigation policy.
    Q_INVOKABLE void setOverlayActive(bool active);

    Q_INVOKABLE void refreshRepositories();
    // Views that read the catalog call this when they show: it is fetched the
    // first time something needs it, not at launch.
    Q_INVOKABLE void requestCatalog();
    Q_INVOKABLE void addRepository(const QString& url);
    Q_INVOKABLE void removeRepository(const QString& url);
    Q_INVOKABLE void setRepositoryEnabled(const QString& url, bool enabled);
    Q_INVOKABLE void refreshDownloadSource();
    Q_INVOKABLE void setDownloadSource(const QString& source);
    Q_INVOKABLE void chooseOnboardingSource(const QString& source);
    Q_INVOKABLE void completeOnboarding();
    Q_INVOKABLE void retryStorage();

signals:
    void currentActiveSectionIndexChanged();

    void launcherAppsChanged();
    void recentlyClosedAppsChanged();
    void currentVisibleAppChanged();
    void loadingModulesChanged();
    void stoppedAppsChanged();
    void navigateToApps();

    // ── Intents ─────────────────────────────────────────────────────────
    // An app asked for a capability the shell provides. Answer with
    // respondToShellIntent(requestId, …). Exactly one handler should connect;
    // the receiver count is what tells the broker anyone is listening at all.
    void shellIntentRequested(const QString& requestId, const QString& intent,
                              const QVariantMap& params, const QString& requesterName);

    // Two or more apps provide the requested capability. `providers` is a sorted
    // list of {moduleName, displayName, iconSource} that the requester never
    // sees and cannot influence.
    void intentChooserRequested(const QString& dispatchId, const QString& intent,
                                const QString& requesterName,
                                const QVariantList& providers);
    void intentChooserDismissed(const QString& dispatchId);

    // Nothing installed provides the requested capability, but the catalog
    // knows packages that would. `candidates` is sorted. The REQUESTER is never
    // told any of this — see ShellIntentInstaller for why.
    // `candidates` is the sorted module names; `details` is the same list with
    // displayName and repositoryUrl attached, so the dialog can show where each
    // package would come from without looking anything up itself.
    void intentInstallOffered(const QString& intent,
                              const QStringList& candidates,
                              const QVariantList& details);

    // `packageName` falls back to the file name if the manifest was unreadable.
    void installFailureNoticeRequested(const QString& packageName,
                                       const QString& errorMessage);

    // A UI app failed to load. Re-emit of UIPluginManager::pluginLoadFailedNotice
    // for the QML overlay (wireIntents() still forwards it to the IntentBroker).
    void loadFailureNoticeRequested(const QString& name, const QString& errorMessage);

    // Something found while Basecamp started -- a module refused, or the
    // installed modules left out -- shown once the window is up.
    void startupNoticeRequested(const QString& title, const QString& name,
                                const QString& message);

    // Trust at install: what the install or upgrade dialog for `name` shows
    // about who signed its plan. See PackageCoordinator.
    void installTrustUpdated(const QString& name, const QVariantMap& trust);

    // Dependency-aware UX. missingDepsPopup + unloadCascade come from
    // UIPluginManager; installGate + uninstallPlan come from
    // PackageCoordinator.
    void missingDepsPopupRequested(const QString& name,
                                   const QVariantList& blockers,
                                   const QString& summary);
    void unloadCascadeConfirmationRequested(const QString& name, const QStringList& loadedDependents);
    // The one uninstall confirmation, for one package or several — a pure
    // re-emit of PackageCoordinator::uninstallPlanRequested, whose comment
    // documents the payload.
    void uninstallPlanRequested(const QVariantMap& plan);

    void dependencyDataReadyChanged();
    // Distinct signal for upgrade/downgrade/reinstall — see
    // PackageCoordinator::upgradeCascadeConfirmationRequested for why we
    // can't reuse the uninstall variant (the dialog needs the target
    // version + UpgradeMode to label itself correctly).
    void upgradeCascadeConfirmationRequested(const QString& name,
                                             const QString& releaseTag,
                                             int mode,
                                             const QStringList& installedDependents,
                                             const QStringList& loadedDependents,
                                             const QVariantList& depChanges,
                                             const QString& requesterName,
                                             bool requesterBundled,
                                             const QString& requestId);

    // Install gate (package_manager_ui-initiated) — pure re-emit
    // of PackageCoordinator::installGateConfirmationRequested. releaseTag is the
    // target version; depChanges is the resolved transitive set the single
    // basecamp dialog lists.
    void optionalGatePreviewUpdated(const QString& name, const QVariantList& changes, bool pending);
    void installGateConfirmationRequested(const QString& name,
                                          const QString& releaseTag,
                                          const QVariantList& depChanges,
                                          const QString& requesterName,
                                          bool requesterBundled,
                                          bool depChangesResolved,
                                          const QString& requestId);
    // Its request ended unanswered: the dialog showing it closes, answering nothing.
    void confirmationWithdrawn(const QString& requestId);

    // MDI coordination (re-emitted from UIPluginManager).
    void pluginWindowRequested(QWidget* widget, const QString& title);
    void pluginWindowRemoveRequested(QWidget* widget);
    // Presentation seam — see IShellHost::onPresentAppRequested.
    void presentAppRequested(QWidget* widget);
    void appStopped(QWidget* widget, const QString& name);

    // A `basecamp://` link could not be opened. Developer-facing text only —
    // never anything the URL supplied, since a browser is a requester with no
    // identity the shell can vouch for and rendering its string would be the
    // spoofing surface the chooser is careful to avoid.
    void linkFailed(const QString& reason);

public:
    // Handed down from main() through Window — see Window::setLinkRaiseHandler.
    void setLinkRaiseHandler(std::function<void()> raise);

signals:

    void repositoriesChanged();
    void trustedPublishersChanged();
    // A change to a trusted publisher that failed; it is as it was.
    void trustedPublisherChangeFailed(const QString& keyName, const QString& message);
    void repositoriesLoadingChanged();
    void downloadSourceChanged();
    void onboardingRequiredChanged();
    void onlineChanged();
    void storageStateChanged();
    void appsLoadingChanged();
    void modulesLoadingChanged();
    void repositoryOperationCompleted(const QString& operation,
                                      const QString& url,
                                      bool success,
                                      const QString& error);

private:
    // Wires the intent signal graph and registers the shell's own provided
    // capabilities. Called once, after UIPluginManager exists.
    void wireIntents();
    void wireIntentsModule();
    void pushInstalledToIntentsModule();
    // intents_module stopped: load it again, and claim the new one.
    void keepIntentsModuleRunning();
    // One of the shell's own intents, from either broker: how many handlers
    // took it (0 = nobody is listening).
    int deliverShellIntent(const QString& dispatchId, const QString& intent,
                           const QVariantMap& params, const QString& requesterName);
    void rebuildIntentRegistry();
    void rebuildInstallableProviders();
    QString repositoryUrlFor(const QString& packageName) const;
    QVariantMap installCandidateDetails(const QString& name) const;

public:
    // What the chooser shows when a user expands a provider row. Resolved
    // ENTIRELY shell-side from the package record, so a provider cannot dress
    // itself up — and deliberately NOT the requester's params, which would put
    // attacker-chosen text in the one dialog whose premise is that the shell
    // drew it.
    Q_INVOKABLE QVariantMap providerDetailsFor(const QString& packageName) const;

private:
    // As respondToShellIntent(); `heard` learns whether the answer reached the
    // requester: false when it was gone, or its request had ended.
    bool answerShellIntent(const QString& requestId, bool ok, const QVariant& data, const QString& error,
                           std::function<void(bool heard)> heard);
    // apps.install / apps.search / packages.install / packages.show to the
    // installer apps (App Manager, Package Manager), which answer only through
    // intents_module; `params` name the package, or the query. `orElse` when
    // nothing can take it.
    void askInstallerApp(const QString& intent, const QVariantMap& params,
                         std::function<void()> orElse);
    void showPackageDetailsFallback(const QString& packageName);
    int beginAppLaunch(const QString& dispatchId, const QVariantMap& params);

    // A link named an app that is not installed. Raises the SHELL's install
    // offer if the catalog knows the name, and does nothing at all if it does
    // not — so a hostile URL cannot make the shell draw a prompt quoting a
    // string it chose.
    void offerInstallForUnknownApp(const QString& appName);

    // Tells the user about a copy of one of Basecamp's own apps found where
    // they install (utils/BuiltInApps.h): each move, and each copy left in
    // place once (m_builtInCopiesLeft).
    void noticeBuiltInCopy(const QString& name, const QString& from, const QString& to, bool stopped);

private slots:
    // Rebuild the inspectors' models from the current manager state. Each
    // is wired to the corresponding *Changed signal on the underlying
    // managers (see ctor) — so a load/unload/stats-tick on
    // CoreModuleManager fans out to refreshCoreModulesModel(), which
    // repopulates m_coreModulesModel in one shot.
    void refreshUiModulesModel();
    void refreshCoreModulesModel();

private:
    void beginModulesLoading();
    void endModulesLoading();

    // Snapshot builders — QVariantList so the composition code (which
    // pulls from three managers) doesn't have to know the model's row
    // layout. Consumed only by the refresh slots above.
    QVariantList buildUiModulesSnapshot() const;
    QVariantList buildCoreModulesSnapshot() const;

    const LogosBasecamp::HostProfile m_hostProfile;
    QSet<QString> m_builtInCopiesLeft;

    // Navigation state.
    int m_currentActiveSectionIndex;
    bool m_overlayActive = false;

    // Intents. Construction order in the ctor is what decides destruction
    // order — see the comment there.
    IntentRegistry*      m_intentRegistry  = nullptr;
    LinkRequestCoordinator* m_linkRequests = nullptr;
    // False until wireIntents() has done its bootstrap rebuild, so the
    // cold-start link gate opens on a registry that has actually seen disk.
    bool m_intentRegistryBootstrapped = false;
    IntentBroker*        m_intentBroker    = nullptr;
    IntentBridgeAdapter* m_intentAdapter   = nullptr;
    UIPluginPresenter*   m_intentPresenter = nullptr;
    // Null when intents_module failed to load. Built next to the adapter for the
    // same reason: bridges tell it when they go.
    IntentsModuleHost*   m_intentsHost     = nullptr;
    IntentChooserQueue*  m_chooserQueue    = nullptr;
    ConfirmationQueue*   m_confirmations   = nullptr;
    std::unique_ptr<ShellIntentEndpoint> m_shellEndpoint;
    std::unique_ptr<ShellIntentChooser>  m_intentChooser;
    std::unique_ptr<ShellIntentInstaller> m_intentInstaller;

    // providerName -> dispatchId, for installs started to satisfy an intent.
    // Keyed by package because that is what the install-completion signals
    // carry; a package installed for any other reason simply is not in here.

    // LogosAPI — shared with all three managers.
    LogosAPI* m_logosAPI;
    ICoreRuntime* m_core; // not owned; owned by main()
    bool m_ownsLogosAPI;

    // Owned children (parent=this). Order matters: coreModuleManager first,
    // uiPluginManager second, packageCoordinator third. See class comment for
    // lifetime reasoning.
    AppsModel*         m_appsModel;
    CoreModuleManager* m_coreModuleManager;
    UIPluginManager*   m_uiPluginManager;
    PackageCoordinator*    m_packageCoordinator;
    ModuleInstanceModel* m_uiModulesModel;
    ModuleInstanceModel* m_coreModulesModel;

    // Settings → Modules Reload overlay (see modulesLoading Q_PROPERTY).
    int  m_modulesLoadingCount = 0;
    bool m_pendingUiModulesRefresh = false;
};
