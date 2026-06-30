#include "MainUIBackend.h"
#include "ModuleFilterProxy.h"
#include "ModuleModel.h"
#include "RepositoryModel.h"
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

    m_moduleModel = new ModuleModel(this);
    m_repositoryModel = new RepositoryModel(this);

    m_uiAppsProxy = new ModuleFilterProxy(this);
    m_uiAppsProxy->setSourceModel(m_moduleModel);
    m_uiAppsProxy->setTypeFilter(QStringLiteral("ui_qml"));
    m_uiAppsProxy->setExcludeMainUi(true);

    m_uiModulesProxy = new ModuleFilterProxy(this);
    m_uiModulesProxy->setSourceModel(m_moduleModel);
    m_uiModulesProxy->setExcludeMainUi(false);
    m_uiModulesProxy->setRequireUiPluginRecord(true);

    m_coreModulesProxy = new ModuleFilterProxy(this);
    m_coreModulesProxy->setSourceModel(m_moduleModel);
    m_coreModulesProxy->setTypeFilter(QStringLiteral("core"));
    m_coreModulesProxy->setRequireCoreModuleRecord(true);

    const auto configureLauncherProxy = [this](ModuleFilterProxy* proxy, int isLoadedFilter) {
        proxy->setSourceModel(m_moduleModel);
        proxy->setRequireUiPluginRecord(true);
        proxy->setExcludedNames({
            QStringLiteral("main_ui"),
            QStringLiteral("package_manager_ui"),
        });
        proxy->setIsLoadedFilter(isLoadedFilter);
    };

    // Source from moduleModel directly — nesting proxies caused runaway filter
    // remaps during startup when IPC callbacks seeded rows while Repeaters bound.
    m_loadedLauncherProxy = new ModuleFilterProxy(this);
    configureLauncherProxy(m_loadedLauncherProxy, 1);

    m_unloadedLauncherProxy = new ModuleFilterProxy(this);
    configureLauncherProxy(m_unloadedLauncherProxy, 0);

    m_requiredPackagesModel = new ModuleFilterProxy(this);
    m_requiredPackagesModel->setSourceModel(m_moduleModel);
    m_requiredPackagesModel->setExcludeMainUi(false);
    m_requiredPackagesModel->setInstallStateFilter(QString());

    m_coreModuleManager = new CoreModuleManager(m_logosAPI, this);
    m_uiPluginManager   = new UIPluginManager(m_logosAPI, m_coreModuleManager, this);
    m_packageCoordinator = new PackageCoordinator(
        m_logosAPI, m_coreModuleManager, m_uiPluginManager,
        m_moduleModel, m_repositoryModel, this);
    m_packageCoordinator->setRequiredPackagesModel(m_requiredPackagesModel);
    m_moduleModel->setInstallRegistry(m_packageCoordinator->installRegistry());

    m_coreModuleManager->setModuleModel(m_moduleModel);
    m_uiPluginManager->setPackageCoordinator(m_packageCoordinator);
    m_uiPluginManager->setModuleModel(m_moduleModel);

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

    connect(m_packageCoordinator, &PackageCoordinator::installConfirmationRequested,
            this,             &MainUIBackend::installConfirmationRequested);
    connect(m_packageCoordinator, &PackageCoordinator::uninstallCascadeConfirmationRequested,
            this,             &MainUIBackend::uninstallCascadeConfirmationRequested);
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

    connect(m_packageCoordinator, &PackageCoordinator::repositoriesLoadingChanged,
            this,             &MainUIBackend::repositoriesLoadingChanged);
    connect(m_packageCoordinator, &PackageCoordinator::appsLoadingChanged,
            this,             &MainUIBackend::appsLoadingChanged);
    connect(m_packageCoordinator, &PackageCoordinator::repositoryOperationCompleted,
            this,             &MainUIBackend::repositoryOperationCompleted);

    QTimer::singleShot(0, this, [this]() {
        m_packageCoordinator->refresh();
    });
    QTimer::singleShot(0, m_coreModuleManager, [this]() {
        m_coreModuleManager->syncKnownModulesToModel();
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
    if (m_currentActiveSectionIndex != index && index >= 0) {
        m_currentActiveSectionIndex = index;
        emit currentActiveSectionIndexChanged();
    }
}

QString      MainUIBackend::currentVisibleApp() const { return m_uiPluginManager->currentVisibleApp(); }
QStringList  MainUIBackend::loadingModules() const   { return m_uiPluginManager->loadingModules(); }

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

void MainUIBackend::cancelPendingAction(const QString& n) {
    m_uiPluginManager->cancelUnloadCascade(n);
    m_packageCoordinator->cancelPendingAction(n);
}

bool MainUIBackend::repositoriesLoading() const { return m_packageCoordinator && m_packageCoordinator->repositoriesLoading(); }

bool MainUIBackend::hasLauncherApps() const
{
    return m_loadedLauncherProxy && m_unloadedLauncherProxy
        && (m_loadedLauncherProxy->rowCount() > 0
            || m_unloadedLauncherProxy->rowCount() > 0);
}
bool MainUIBackend::appsLoading() const
{ return !m_packageCoordinator || m_packageCoordinator->appsLoading(); }
void MainUIBackend::refreshRepositories()                                  { m_packageCoordinator->refreshRepositories(); }
void MainUIBackend::refreshAppCatalog()                                    { m_packageCoordinator->refresh(); }
void MainUIBackend::addRepository(const QString& url)                      { m_packageCoordinator->addRepository(url); }
void MainUIBackend::removeRepository(const QString& url)                   { m_packageCoordinator->removeRepository(url); }
void MainUIBackend::setRepositoryEnabled(const QString& url, bool enabled) { m_packageCoordinator->setRepositoryEnabled(url, enabled); }

void    MainUIBackend::refreshCoreModules()
{
    m_coreModuleManager->refresh();
    m_coreModuleManager->syncKnownModulesToModel();
}
QString MainUIBackend::getCoreModuleMethods(const QString& n)       { return m_coreModuleManager->getMethods(n); }
QString MainUIBackend::getCoreModuleEvents(const QString& n)        { return m_coreModuleManager->getEvents(n); }
QString MainUIBackend::callCoreModuleMethod(const QString& n,
                                             const QString& m,
                                             const QString& a)      { return m_coreModuleManager->callMethod(n, m, a); }

QString      MainUIBackend::buildVersion() const    { return LogosBasecampBuildInfo::version(); }
bool         MainUIBackend::isPortableBuild() const { return LogosBasecampBuildInfo::isPortableBuild(); }
QVariantList MainUIBackend::buildCommits() const    { return LogosBasecampBuildInfo::commits(); }

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
