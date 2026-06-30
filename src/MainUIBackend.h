#pragma once

#include "InstallEnums.h"
#include "ModuleFilterProxy.h"
#include "ModuleModel.h"
#include "RepositoryModel.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include "logos_api.h"

class ModuleModel;
class RepositoryModel;
class CoreModuleManager;
class PackageCoordinator;
class QWidget;
class UIPluginManager;

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
// when syncing rows into ModuleModel. Qt's reverse-order child
// destruction tears PackageCoordinator → UIPluginManager → CoreModuleManager,
// which matches the data-flow dependencies (PackageCoordinator stops emitting
// before UIPluginManager tears down its widgets, which stop before the C
// API handle goes away).
//
// Everything the QML layer calls (`backend.loadUiModule(...)`, etc.) is a
// one-line delegation into one of the three managers. Manager signals the QML
// layer cares about are re-emitted here via signal-to-signal connect.
// Navigation is the only behavior that lives on this class itself.
class MainUIBackend : public QObject {
    Q_OBJECT

    // Navigation
    Q_PROPERTY(int currentActiveSectionIndex READ currentActiveSectionIndex WRITE setCurrentActiveSectionIndex NOTIFY currentActiveSectionIndexChanged)

    // Prebuilt filter proxies over ModuleModel (one preset per view).
    Q_PROPERTY(ModuleFilterProxy* uiAppsProxy READ uiAppsProxy CONSTANT)
    Q_PROPERTY(ModuleFilterProxy* uiModulesProxy READ uiModulesProxy CONSTANT)
    Q_PROPERTY(ModuleFilterProxy* coreModulesProxy READ coreModulesProxy CONSTANT)
    Q_PROPERTY(ModuleFilterProxy* loadedLauncherProxy READ loadedLauncherProxy CONSTANT)
    Q_PROPERTY(ModuleFilterProxy* unloadedLauncherProxy READ unloadedLauncherProxy CONSTANT)
    Q_PROPERTY(ModuleFilterProxy* requiredPackagesModel READ requiredPackagesModel CONSTANT)

    // App Launcher
    Q_PROPERTY(QString currentVisibleApp READ currentVisibleApp NOTIFY currentVisibleAppChanged)
    Q_PROPERTY(QStringList loadingModules READ loadingModules NOTIFY loadingModulesChanged)

    // Package repositories
    Q_PROPERTY(RepositoryModel* repositoriesModel READ repositoriesModel CONSTANT)
    Q_PROPERTY(bool repositoriesLoading READ repositoriesLoading NOTIFY repositoriesLoadingChanged)

    // App Manager loading state — true until the first catalog populate.
    Q_PROPERTY(bool appsLoading READ appsLoading NOTIFY appsLoadingChanged)

    // Build info (baked in at nix build time). See Dashboard view.
    Q_PROPERTY(QString buildVersion READ buildVersion CONSTANT)
    Q_PROPERTY(bool isPortableBuild READ isPortableBuild CONSTANT)
    Q_PROPERTY(QVariantList buildCommits READ buildCommits CONSTANT)

    // Sidebar tooltip — bridged to the overlay widget so it can render
    // outside the narrow sidebar QQuickWidget.
    Q_PROPERTY(QString sidebarTooltipText READ sidebarTooltipText WRITE setSidebarTooltipText NOTIFY sidebarTooltipChanged)
    Q_PROPERTY(qreal sidebarTooltipY READ sidebarTooltipY WRITE setSidebarTooltipY NOTIFY sidebarTooltipChanged)

public:
    explicit MainUIBackend(LogosAPI* logosAPI = nullptr, QObject* parent = nullptr);
    ~MainUIBackend() override;

    // Navigation — lives on this class.
    int currentActiveSectionIndex() const;

    // Delegations to UIPluginManager.
    QString      currentVisibleApp() const;
    QStringList  loadingModules() const;

    // Build info accessors (see Q_PROPERTY declarations above).
    QString buildVersion() const;
    bool isPortableBuild() const;
    QVariantList buildCommits() const;

    // Sidebar tooltip accessors.
    QString sidebarTooltipText() const { return m_sidebarTooltipText; }
    qreal sidebarTooltipY() const { return m_sidebarTooltipY; }

    bool repositoriesLoading() const;
    bool hasLauncherApps() const;
    bool appsLoading() const;
    void setSidebarTooltipText(const QString& text);
    void setSidebarTooltipY(qreal y);

    // Accessors for C++ coordination code (MdiView etc.) that needs a handle
    // to the managers directly. QML goes through the delegating slots/signals.
    CoreModuleManager* coreModuleManager() const { return m_coreModuleManager; }
    UIPluginManager*   uiPluginManager()   const { return m_uiPluginManager; }
    PackageCoordinator*    packageCoordinator()    const { return m_packageCoordinator; }
    ModuleModel*           moduleModel()            const { return m_moduleModel; }
    RepositoryModel*       repositoriesModel()      const { return m_repositoryModel; }
    ModuleFilterProxy*     uiAppsProxy()            const { return m_uiAppsProxy; }
    ModuleFilterProxy*     uiModulesProxy()         const { return m_uiModulesProxy; }
    ModuleFilterProxy*     coreModulesProxy()       const { return m_coreModulesProxy; }
    ModuleFilterProxy*     loadedLauncherProxy()    const { return m_loadedLauncherProxy; }
    ModuleFilterProxy*     unloadedLauncherProxy()  const { return m_unloadedLauncherProxy; }
    ModuleFilterProxy*     requiredPackagesModel()  const { return m_requiredPackagesModel; }

public slots:
    // Navigation
    void setCurrentActiveSectionIndex(int index);

    // UI Module operations — delegated to UIPluginManager.
    void loadUiModule(const QString& moduleName);
    void unloadUiModule(const QString& moduleName);
    void activateApp(const QString& appName);

    // Friendly module label, resolved from the catalog with fallback to `name`.
    Q_INVOKABLE QString displayNameFor(const QString& moduleName) const;

    // Install flow — delegated to PackageCoordinator.
    Q_INVOKABLE void installPluginFromPath(const QString& filePath);
    Q_INVOKABLE void openInstallPluginDialog();

    // Uninstall flow — delegated to PackageCoordinator.
    Q_INVOKABLE void uninstallUiModule(const QString& moduleName);
    Q_INVOKABLE void uninstallCoreModule(const QString& moduleName);

    // Cascade confirmation flow. Local unload cascade lives on
    // UIPluginManager; uninstall/upgrade cascade lives on PackageCoordinator.
    // cancelPendingAction fans out to both so the un-involved one no-ops.
    Q_INVOKABLE void confirmUnloadCascade(const QString& moduleName);
    Q_INVOKABLE void confirmUninstallCascade(const QString& moduleName);
    Q_INVOKABLE void confirmUninstallMultiCascade(const QStringList& moduleNames);
    Q_INVOKABLE void cancelMultiUninstall(const QStringList& moduleNames);
    Q_INVOKABLE void cancelPendingAction(const QString& moduleName);

    // Install confirmation flow — delegated to PackageCoordinator.
    Q_INVOKABLE void confirmInstall();
    Q_INVOKABLE void cancelInstall();

    // App-Manager catalog open — delegated to PackageCoordinator.
    Q_INVOKABLE void openApp(const QString& name,
                             const QString& repositoryUrl,
                             const QVariantMap& versionPins = QVariantMap(),
                             bool allowFastLaunch = true);
    Q_INVOKABLE void confirmCatalogInstall(const QString& name,
                                           const QString& repositoryUrl,
                                           const QVariantMap& versionPins = QVariantMap());
    Q_INVOKABLE void notifyAddApplicationDialogClosed();

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
    void onPluginWindowClosed(const QString& pluginName);
    void setCurrentVisibleApp(const QString& pluginName);

    Q_INVOKABLE void refreshRepositories();
    Q_INVOKABLE void refreshAppCatalog();
    Q_INVOKABLE void addRepository(const QString& url);
    Q_INVOKABLE void removeRepository(const QString& url);
    Q_INVOKABLE void setRepositoryEnabled(const QString& url, bool enabled);

signals:
    void currentActiveSectionIndexChanged();

    // App-Manager dialog + install lifecycle. See PackageCoordinator for
    // the contract — these are pure re-emits.
    void requestOpenAddApplicationDialog(const QVariantMap& metadata);
    void addApplicationDataUpdated(const QVariantMap& metadata);
    void launchAppRequested(const QString& name);
    void catalogInstallStageChanged(const QString& name, InstallStage::Value stage);
    void catalogInstallFinished(const QString& name);
    void catalogInstallFailed(const QString& name, const QString& error);
    void currentVisibleAppChanged();
    void loadingModulesChanged();
    void navigateToApps();

    // Dependency-aware UX. missingDepsPopup + unloadCascade come from
    // UIPluginManager; installConfirm + uninstallCascade come from
    // PackageCoordinator.
    void missingDepsPopupRequested(const QString& name, const QStringList& missing);
    void unloadCascadeConfirmationRequested(const QString& name, const QStringList& loadedDependents);
    void uninstallCascadeConfirmationRequested(const QString& name,
                                               const QStringList& installedDependents,
                                               const QStringList& loadedDependents);
    // Distinct signal for upgrade/downgrade/reinstall — see
    // PackageCoordinator::upgradeCascadeConfirmationRequested for why we
    // can't reuse the uninstall variant (the dialog needs the target
    // version + UpgradeMode to label itself correctly).
    void upgradeCascadeConfirmationRequested(const QString& name,
                                             const QString& releaseTag,
                                             int mode,
                                             const QStringList& installedDependents,
                                             const QStringList& loadedDependents);
    void uninstallMultiCascadeConfirmationRequested(const QStringList& names,
                                                    const QStringList& installedDependents,
                                                    const QStringList& loadedDependents);

    // Install confirmation — emitted when the user picks an LGX file and we've
    // inspected it. metadata contains name, version, type, signatureStatus, etc.
    void installConfirmationRequested(const QVariantMap& metadata);

    // MDI coordination (re-emitted from UIPluginManager).
    void pluginWindowRequested(QWidget* widget, const QString& title);
    void pluginWindowRemoveRequested(QWidget* widget);
    void pluginWindowActivateRequested(QWidget* widget);
    void sidebarTooltipChanged();

    void repositoriesLoadingChanged();
    void appsLoadingChanged();
    void repositoryOperationCompleted(const QString& operation,
                                      const QString& url,
                                      bool success,
                                      const QString& error);

private:
    // Navigation state — the only state this facade class holds.
    int m_currentActiveSectionIndex;

    // LogosAPI — shared with all three managers.
    LogosAPI* m_logosAPI;
    bool m_ownsLogosAPI;

    // Owned children (parent=this). Order matters: coreModuleManager first,
    // uiPluginManager second, packageCoordinator third. See class comment for
    // lifetime reasoning.
    ModuleModel*           m_moduleModel;
    RepositoryModel*       m_repositoryModel;
    ModuleFilterProxy*     m_uiAppsProxy;
    ModuleFilterProxy*     m_uiModulesProxy;
    ModuleFilterProxy*     m_coreModulesProxy;
    ModuleFilterProxy*     m_loadedLauncherProxy;
    ModuleFilterProxy*     m_unloadedLauncherProxy;
    ModuleFilterProxy*     m_requiredPackagesModel;
    CoreModuleManager* m_coreModuleManager;
    UIPluginManager*   m_uiPluginManager;
    PackageCoordinator*    m_packageCoordinator;

    QString m_sidebarTooltipText;
    qreal m_sidebarTooltipY = 0;
};
