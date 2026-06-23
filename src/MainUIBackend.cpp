#include "MainUIBackend.h"
#include "AppsFilterProxy.h"
#include "AppsModel.h"
#include "CoreModuleManager.h"
#include "UIPluginManager.h"
#include "PackageCoordinator.h"
#include "BuildInfo.h"

#include <QDebug>
#include <QTimer>

MainUIBackend::MainUIBackend(LogosAPI* logosAPI, QObject* parent)
    : QObject(parent)
    , m_currentActiveSectionIndex(0)
    , m_logosAPI(logosAPI)
    , m_ownsLogosAPI(false)
    , m_coreModuleManager(nullptr)
    , m_uiPluginManager(nullptr)
    , m_packageCoordinator(nullptr)
{
    if (!m_logosAPI) {
        m_logosAPI = new LogosAPI("core", this);
        m_ownsLogosAPI = true;
    }

    // Order matters: CoreModuleManager must exist before UIPluginManager so
    // the latter's ctor can receive a valid pointer; UIPluginManager must
    // exist before PackageCoordinator for the same reason. Qt tears children
    // down in reverse order at destruction, so PackageCoordinator dies first
    // (stops talking to the module), then UIPluginManager (tears down
    // widgets while CoreModuleManager's C API handle is still valid).


    m_appsModel         = new AppsModel(this);

    m_uiAppsProxy       = new AppsFilterProxy(this);
    m_uiAppsProxy->setSourceModel(m_appsModel);
    m_uiAppsProxy->setTypeFilter(QStringLiteral("ui_qml"));
    m_uiAppsProxy->setExcludeMainUi(true);

    m_requiredPackagesModel = new AppsFilterProxy(this);
    m_requiredPackagesModel->setSourceModel(m_appsModel);
    m_requiredPackagesModel->setExcludeMainUi(false);
    m_requiredPackagesModel->setInstallStateFilter(QString());

    m_coreModuleManager = new CoreModuleManager(m_logosAPI, this);
    m_uiPluginManager   = new UIPluginManager(m_logosAPI, m_coreModuleManager, this);
    m_packageCoordinator    = new PackageCoordinator(m_logosAPI, m_coreModuleManager, m_uiPluginManager, m_appsModel, this);
    m_packageCoordinator->setRequiredPackagesModel(m_requiredPackagesModel);
    m_appsModel->setInstallRegistry(m_packageCoordinator->installRegistry());

    // Setter-injection closes the cycle — UIPluginManager queries
    // PackageCoordinator for installType / missing-deps when building its
    // uiModules() list, and consumes uiPluginsFetched for its UI-specific
    // metadata cache. See UIPluginManager::setPackageCoordinator for the signal
    // connections it sets up internally.
    m_uiPluginManager->setPackageCoordinator(m_packageCoordinator);

    // Forward manager signals into our own signals of the same name. QML
    // binds to these; by funneling through the facade we keep a stable
    // surface regardless of which manager emitted them.
    //
    // UIPluginManager drives uiModulesChanged/launcherAppsChanged on load/
    // unload events; PackageCoordinator's own uiModulesChanged/launcherAppsChanged
    // already flow through UIPluginManager (wired in setPackageCoordinator) so
    // we only need to listen to UIPluginManager here.
    connect(m_uiPluginManager, &UIPluginManager::uiModulesChanged,
            this,              &MainUIBackend::uiModulesChanged);
    connect(m_uiPluginManager, &UIPluginManager::launcherAppsChanged,
            this,              &MainUIBackend::launcherAppsChanged);
    connect(m_uiPluginManager, &UIPluginManager::loadingModulesChanged,
            this,              &MainUIBackend::loadingModulesChanged);
    connect(m_uiPluginManager, &UIPluginManager::currentVisibleAppChanged,
            this,              &MainUIBackend::currentVisibleAppChanged);
    connect(m_uiPluginManager, &UIPluginManager::navigateToApps,
            this,              &MainUIBackend::navigateToApps);
    connect(m_uiPluginManager, &UIPluginManager::missingDepsPopupRequested,
            this,              &MainUIBackend::missingDepsPopupRequested);
    connect(m_uiPluginManager, &UIPluginManager::unloadCascadeConfirmationRequested,
            this,              &MainUIBackend::unloadCascadeConfirmationRequested);
    connect(m_uiPluginManager, &UIPluginManager::pluginWindowRequested,
            this,              &MainUIBackend::pluginWindowRequested);
    connect(m_uiPluginManager, &UIPluginManager::pluginWindowRemoveRequested,
            this,              &MainUIBackend::pluginWindowRemoveRequested);
    connect(m_uiPluginManager, &UIPluginManager::pluginWindowActivateRequested,
            this,              &MainUIBackend::pluginWindowActivateRequested);

    // PackageCoordinator emits its own dialog-request signals; forward both.
    connect(m_packageCoordinator, &PackageCoordinator::installConfirmationRequested,
            this,             &MainUIBackend::installConfirmationRequested);
    connect(m_packageCoordinator, &PackageCoordinator::uninstallCascadeConfirmationRequested,
            this,             &MainUIBackend::uninstallCascadeConfirmationRequested);
    // Distinct upgrade/downgrade/reinstall cascade signal — the dialog
    // shape is the same as the uninstall variant, but the title + body
    // need the target releaseTag + UpgradeMode (so a downgrade doesn't
    // look like a bare uninstall). PackageCoordinator emits this from
    // onBeforeUpgrade; OverlayDialogs.qml renders it via the
    // "upgradeCascade" mode of ConfirmationDialog.
    connect(m_packageCoordinator, &PackageCoordinator::upgradeCascadeConfirmationRequested,
            this,             &MainUIBackend::upgradeCascadeConfirmationRequested);
    connect(m_packageCoordinator, &PackageCoordinator::uninstallMultiCascadeConfirmationRequested,
            this,             &MainUIBackend::uninstallMultiCascadeConfirmationRequested);
    connect(m_packageCoordinator, &PackageCoordinator::requestOpenAddApplicationDialog,
            this,             &MainUIBackend::requestOpenAddApplicationDialog);
    connect(m_packageCoordinator, &PackageCoordinator::addApplicationDataUpdated,
            this,             &MainUIBackend::addApplicationDataUpdated);
    connect(m_packageCoordinator, &PackageCoordinator::launchAppRequested,
            this,             &MainUIBackend::launchAppRequested);
    connect(m_packageCoordinator, &PackageCoordinator::catalogInstallStageChanged,
            this,             &MainUIBackend::catalogInstallStageChanged);
    connect(m_packageCoordinator, &PackageCoordinator::catalogInstallFinished,
            this,             &MainUIBackend::catalogInstallFinished);
    connect(m_packageCoordinator, &PackageCoordinator::catalogInstallFailed,
            this,             &MainUIBackend::catalogInstallFailed);

    // Package repositories — pure re-emits so QML binding to backend.*
    connect(m_packageCoordinator, &PackageCoordinator::repositoriesChanged,
            this,             &MainUIBackend::repositoriesChanged);
    connect(m_packageCoordinator, &PackageCoordinator::repositoriesLoadingChanged,
            this,             &MainUIBackend::repositoriesLoadingChanged);
    connect(m_packageCoordinator, &PackageCoordinator::appsLoadingChanged,
            this,             &MainUIBackend::appsLoadingChanged);
    connect(m_packageCoordinator, &PackageCoordinator::repositoryOperationCompleted,
            this,             &MainUIBackend::repositoryOperationCompleted);

    // Any of the three managers can trigger coreModulesChanged:
    //   * CoreModuleManager on stats-tick / refresh
    //   * UIPluginManager on cascade-induced state changes (re-emits
    //     PackageCoordinator's coreModulesChanged as part of that wiring)
    // Qt coalesces redundant property-change notifies within a frame so the
    // multi-connect doesn't cause visible flicker.
    connect(m_uiPluginManager,    &UIPluginManager::coreModulesChanged,
            this,                 &MainUIBackend::coreModulesChanged);
    connect(m_coreModuleManager,  &CoreModuleManager::coreModulesChanged,
            this,                 &MainUIBackend::coreModulesChanged);

    // Kick the first catalog scan now that all wiring is in place. We do
    // this AFTER setPackageCoordinator (and its signal connections) so the
    // resulting uiPluginsFetched / uiModulesChanged land on live slots.
    QTimer::singleShot(0, this, [this]() {
        m_packageCoordinator->refresh();
    });

    qDebug() << "MainUIBackend created";
}

MainUIBackend::~MainUIBackend() = default;

int MainUIBackend::currentActiveSectionIndex() const
{
    return m_currentActiveSectionIndex;
}

void MainUIBackend::setCurrentActiveSectionIndex(int index)
{
    // Section list is owned by QML (SidebarPanel). The upper bound is
    // self-policed there; we only guard against negative indices.
    // Per-section side effects (e.g., the Modules-view auto-refresh) live
    // in the QML view that becomes visible, not here.
    if (m_currentActiveSectionIndex != index && index >= 0) {
        m_currentActiveSectionIndex = index;
        emit currentActiveSectionIndexChanged();
    }
}

// --- coreModules() composer ------------------------------------------------
//
// coreModules is the one QML-visible property that spans multiple managers.
// Known + loaded + stats come from CoreModuleManager (raw liblogos state);
// installType comes from PackageCoordinator (populated during its dep-info
// refresh). We compose here so neither manager has to know about the other's
// schema.
QVariantList MainUIBackend::coreModules() const
{
    QVariantList modules;
    if (!m_coreModuleManager) return modules;

    const QStringList known  = m_coreModuleManager->knownModules();
    const QStringList loaded = m_coreModuleManager->loadedModules();

    for (const QString& name : known) {
        QVariantMap module;
        module["name"] = name;
        module["displayName"] = m_packageCoordinator ? m_packageCoordinator->displayNameFor(name) : name;
        module["isLoaded"] = loaded.contains(name);
        // installType populated lazily by refreshDependencyInfo's full-scan
        // pass on PackageCoordinator. Empty means "not known yet" — QML treats
        // that as a non-user module and hides Uninstall, which is the safe
        // default.
        module["installType"] = m_packageCoordinator ? m_packageCoordinator->installType(name) : QString();

        const QVariantMap stats = m_coreModuleManager->moduleStats(name);
        if (!stats.isEmpty()) {
            module["cpu"] = stats["cpu"];
            module["memory"] = stats["memory"];
        } else {
            module["cpu"] = "0.0";
            module["memory"] = "0.0";
        }

        modules.append(module);
    }

    return modules;
}

// --- Manager delegations ---------------------------------------------------
//
// Each slot is a one-liner routing to the right manager. These stay on
// MainUIBackend so the QML `backend.foo(...)` contract is untouched by the
// refactor (QML still sees one receiver).

QVariantList MainUIBackend::uiModules() const        { return m_uiPluginManager->uiModules(); }
QVariantList MainUIBackend::launcherApps() const     { return m_uiPluginManager->launcherApps(); }
QString      MainUIBackend::currentVisibleApp() const{ return m_uiPluginManager->currentVisibleApp(); }
QStringList  MainUIBackend::loadingModules() const   { return m_uiPluginManager->loadingModules(); }

// UIPluginManager — UI plugin widget lifecycle + local unload cascade.
void MainUIBackend::loadUiModule(const QString& n)            { m_uiPluginManager->loadUiModule(n); }
void MainUIBackend::unloadUiModule(const QString& n)          { m_uiPluginManager->unloadUiModule(n); }
void MainUIBackend::activateApp(const QString& n)             { m_uiPluginManager->activateApp(n); }
void MainUIBackend::confirmUnloadCascade(const QString& n)    { m_uiPluginManager->confirmUnloadCascade(n); }
void MainUIBackend::loadCoreModule(const QString& n)          { m_uiPluginManager->loadCoreModule(n); }
void MainUIBackend::unloadCoreModule(const QString& n)        { m_uiPluginManager->unloadCoreModule(n); }
void MainUIBackend::refreshUiModules()                        { m_uiPluginManager->refreshUiModules(); }
void MainUIBackend::onAppLauncherClicked(const QString& n)    { m_uiPluginManager->onAppLauncherClicked(n); }
void MainUIBackend::onPluginWindowClosed(const QString& n)    { m_uiPluginManager->onPluginWindowClosed(n); }
void MainUIBackend::setCurrentVisibleApp(const QString& n)    { m_uiPluginManager->setCurrentVisibleApp(n); }

// PackageCoordinator — package_manager IPC and package-lifecycle cascade.
QString MainUIBackend::displayNameFor(const QString& n) const {
    return m_packageCoordinator ? m_packageCoordinator->displayNameFor(n) : n;
}
void MainUIBackend::installPluginFromPath(const QString& p)   { m_packageCoordinator->installPluginFromPath(p); }
void MainUIBackend::openInstallPluginDialog()                 { m_packageCoordinator->openInstallPluginDialog(); }
void MainUIBackend::uninstallUiModule(const QString& n)       { m_packageCoordinator->uninstallUiModule(n); }
void MainUIBackend::uninstallCoreModule(const QString& n)     { m_packageCoordinator->uninstallCoreModule(n); }
void MainUIBackend::confirmUninstallCascade(const QString& n) { m_packageCoordinator->confirmUninstallCascade(n); }
void MainUIBackend::confirmUninstallMultiCascade(const QStringList& names) { m_packageCoordinator->confirmUninstallMultiCascade(names); }
void MainUIBackend::cancelMultiUninstall(const QStringList& names)         { m_packageCoordinator->cancelMultiUninstall(names); }
void MainUIBackend::confirmInstall()                          { m_packageCoordinator->confirmInstall(); }
void MainUIBackend::cancelInstall()                           { m_packageCoordinator->cancelInstall(); }
void MainUIBackend::openApp(const QString& name, const QString& repositoryUrl, const QVariantMap& versionPins, bool allowFastLaunch)
{ m_packageCoordinator->openApp(name, repositoryUrl, versionPins, allowFastLaunch); }
void MainUIBackend::confirmCatalogInstall(const QString& name, const QString& repositoryUrl, const QVariantMap& versionPins)
{ m_packageCoordinator->confirmCatalogInstall(name, repositoryUrl, versionPins); }
void MainUIBackend::notifyAddApplicationDialogClosed()
{ m_packageCoordinator->notifyAddApplicationDialogClosed(); }

// cancelPendingAction is the one slot that doesn't route to a single manager:
// a pending action lives on either UIPluginManager (local unload cascade) or
// PackageCoordinator (uninstall/upgrade cascade) but not both. Fan out to both —
// the un-involved manager no-ops on name-mismatch. This preserves the QML
// contract (single `backend.cancelPendingAction(name)` call for either dialog).
void MainUIBackend::cancelPendingAction(const QString& n) {
    m_uiPluginManager->cancelUnloadCascade(n);
    m_packageCoordinator->cancelPendingAction(n);
}

// Package repositories — delegations + cache pass-through.
QVariantList MainUIBackend::repositories() const        { return m_packageCoordinator->repositories(); }
bool         MainUIBackend::repositoriesLoading() const { return m_packageCoordinator->repositoriesLoading(); }
bool MainUIBackend::appsLoading() const
{ return !m_packageCoordinator || m_packageCoordinator->appsLoading(); }
void MainUIBackend::refreshRepositories()                                  { m_packageCoordinator->refreshRepositories(); }
void MainUIBackend::refreshAppCatalog()                                    { m_packageCoordinator->refresh(); }
void MainUIBackend::addRepository(const QString& url)                      { m_packageCoordinator->addRepository(url); }
void MainUIBackend::removeRepository(const QString& url)                   { m_packageCoordinator->removeRepository(url); }
void MainUIBackend::setRepositoryEnabled(const QString& url, bool enabled) { m_packageCoordinator->setRepositoryEnabled(url, enabled); }

// --- CoreModuleManager delegations ----------------------------------------

void    MainUIBackend::refreshCoreModules()                         { m_coreModuleManager->refresh(); }
QString MainUIBackend::getCoreModuleMethods(const QString& n)       { return m_coreModuleManager->getMethods(n); }
QString MainUIBackend::getCoreModuleEvents(const QString& n)        { return m_coreModuleManager->getEvents(n); }
QString MainUIBackend::callCoreModuleMethod(const QString& n,
                                             const QString& m,
                                             const QString& a)      { return m_coreModuleManager->callMethod(n, m, a); }

// --- Build info -----------------------------------------------------------
//
// Thin QML-facing wrappers over the shared LogosBasecampBuildInfo helper
// (app/utils/BuildInfo.h), which reads the nix-generated logos_build_info.h.

QString      MainUIBackend::buildVersion() const    { return LogosBasecampBuildInfo::version(); }
bool         MainUIBackend::isPortableBuild() const { return LogosBasecampBuildInfo::isPortableBuild(); }
QVariantList MainUIBackend::buildCommits() const    { return LogosBasecampBuildInfo::commits(); }

// --- Sidebar tooltip -------------------------------------------------------

void MainUIBackend::setSidebarTooltipText(const QString& text) {
    if (m_sidebarTooltipText != text) {
        m_sidebarTooltipText = text;
        emit sidebarTooltipChanged();
    }
}

void MainUIBackend::setSidebarTooltipY(qreal y) {
    if (m_sidebarTooltipY != y) {
        m_sidebarTooltipY = y;
        emit sidebarTooltipChanged();
    }
}
