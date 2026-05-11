#include "MainUIBackend.h"
#include "CoreModuleManager.h"
#include "UIPluginManager.h"
#include "PackageCoordinator.h"
#include "BuildInfo.h"

#include <QDebug>

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

    initializeSections();

    // Order matters: CoreModuleManager must exist before UIPluginManager so
    // the latter's ctor can receive a valid pointer; UIPluginManager must
    // exist before PackageCoordinator for the same reason. Qt tears children
    // down in reverse order at destruction, so PackageCoordinator dies first
    // (stops talking to the module), then UIPluginManager (tears down
    // widgets while CoreModuleManager's C API handle is still valid).
    m_coreModuleManager = new CoreModuleManager(m_logosAPI, this);
    m_uiPluginManager   = new UIPluginManager(m_logosAPI, m_coreModuleManager, this);
    m_packageCoordinator    = new PackageCoordinator(m_logosAPI, m_coreModuleManager, m_uiPluginManager, this);

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
    connect(m_packageCoordinator, &PackageCoordinator::uninstallMultiCascadeConfirmationRequested,
            this,             &MainUIBackend::uninstallMultiCascadeConfirmationRequested);

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
    m_packageCoordinator->refresh();

    qDebug() << "MainUIBackend created";
}

MainUIBackend::~MainUIBackend() = default;

void MainUIBackend::initializeSections()
{
    auto makeSection = [](const QString& name, const QString& iconPath, const QString& type) {
        QVariantMap section;
        section["name"] = name;
        section["iconPath"] = iconPath;
        section["type"] = type;
        return section;
    };

    m_sections = QVariantList{
        makeSection("Apps", "qrc:/icons/tent.png", "workspace"),
        makeSection("Dashboard", "qrc:/icons/dashboard.png", "view"),
        makeSection("Modules", "qrc:/icons/module.png", "view"),
        makeSection("Settings", "qrc:/icons/settings.png", "view")
    };
}

int MainUIBackend::currentActiveSectionIndex() const
{
    return m_currentActiveSectionIndex;
}

void MainUIBackend::setCurrentActiveSectionIndex(int index)
{
    // Valid indices: 0-3 (Apps, Dashboard, Modules, Settings)
    if (m_currentActiveSectionIndex != index && index >= 0 && index < m_sections.size()) {
        m_currentActiveSectionIndex = index;
        emit currentActiveSectionIndexChanged();

        // On entering the Modules view, kick a refresh of both lists so the
        // user sees up-to-date state. Both managers' refresh paths are
        // non-blocking.
        const QVariantMap section = m_sections[index].toMap();
        const QString name = section.value("name").toString();
        if (name == "Modules") {
            m_uiPluginManager->refreshUiModules();
            m_coreModuleManager->refresh();
        }
    }
}

QVariantList MainUIBackend::sections() const
{
    return m_sections;
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
void MainUIBackend::installPluginFromPath(const QString& p)   { m_packageCoordinator->installPluginFromPath(p); }
void MainUIBackend::openInstallPluginDialog()                 { m_packageCoordinator->openInstallPluginDialog(); }
void MainUIBackend::uninstallUiModule(const QString& n)       { m_packageCoordinator->uninstallUiModule(n); }
void MainUIBackend::uninstallCoreModule(const QString& n)     { m_packageCoordinator->uninstallCoreModule(n); }
void MainUIBackend::confirmUninstallCascade(const QString& n) { m_packageCoordinator->confirmUninstallCascade(n); }
void MainUIBackend::confirmUninstallMultiCascade(const QStringList& names) { m_packageCoordinator->confirmUninstallMultiCascade(names); }
void MainUIBackend::cancelMultiUninstall(const QStringList& names)         { m_packageCoordinator->cancelMultiUninstall(names); }
void MainUIBackend::confirmInstall()                          { m_packageCoordinator->confirmInstall(); }
void MainUIBackend::cancelInstall()                           { m_packageCoordinator->cancelInstall(); }

// cancelPendingAction is the one slot that doesn't route to a single manager:
// a pending action lives on either UIPluginManager (local unload cascade) or
// PackageCoordinator (uninstall/upgrade cascade) but not both. Fan out to both —
// the un-involved manager no-ops on name-mismatch. This preserves the QML
// contract (single `backend.cancelPendingAction(name)` call for either dialog).
void MainUIBackend::cancelPendingAction(const QString& n) {
    m_uiPluginManager->cancelUnloadCascade(n);
    m_packageCoordinator->cancelPendingAction(n);
}

// --- CoreModuleManager delegations ----------------------------------------

void    MainUIBackend::refreshCoreModules()                         { m_coreModuleManager->refresh(); }
QString MainUIBackend::getCoreModuleMethods(const QString& n)       { return m_coreModuleManager->getMethods(n); }
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
