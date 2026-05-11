#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include "logos_api.h"

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
// for its uiModules() / launcherApps() builders. Qt's reverse-order child
// destruction tears PackageCoordinator → UIPluginManager → CoreModuleManager,
// which matches the data-flow dependencies (PackageCoordinator stops emitting
// before UIPluginManager tears down its widgets, which stop before the C
// API handle goes away).
//
// Everything the QML layer calls (`backend.loadUiModule(...)`, etc.) is a
// one-line delegation into one of the three managers. Every signal the QML
// layer listens to (`uiModulesChanged`, `coreModulesChanged`, …) is
// re-emitted here from whichever manager is driving that change via
// signal-to-signal connect. Navigation is the only behavior that lives on
// this class itself.
class MainUIBackend : public QObject {
    Q_OBJECT

    // Navigation
    Q_PROPERTY(int currentActiveSectionIndex READ currentActiveSectionIndex WRITE setCurrentActiveSectionIndex NOTIFY currentActiveSectionIndexChanged)
    Q_PROPERTY(QVariantList sections READ sections CONSTANT)

    // UI Modules (Apps)
    Q_PROPERTY(QVariantList uiModules READ uiModules NOTIFY uiModulesChanged)

    // Core Modules — composed here from all three managers (known list +
    // stats from CoreModuleManager, installType from PackageCoordinator).
    Q_PROPERTY(QVariantList coreModules READ coreModules NOTIFY coreModulesChanged)

    // App Launcher
    Q_PROPERTY(QVariantList launcherApps READ launcherApps NOTIFY launcherAppsChanged)
    Q_PROPERTY(QString currentVisibleApp READ currentVisibleApp NOTIFY currentVisibleAppChanged)
    Q_PROPERTY(QStringList loadingModules READ loadingModules NOTIFY loadingModulesChanged)

    // Build info (baked in at nix build time). See Dashboard view.
    //   * buildVersion: VERSION file contents (empty when not baked in).
    //   * isPortableBuild: true for distributed/release builds, false for dev.
    //   * buildCommits: list of { name, commit } for basecamp + each flake input.
    Q_PROPERTY(QString buildVersion READ buildVersion CONSTANT)
    Q_PROPERTY(bool isPortableBuild READ isPortableBuild CONSTANT)
    Q_PROPERTY(QVariantList buildCommits READ buildCommits CONSTANT)

public:
    explicit MainUIBackend(LogosAPI* logosAPI = nullptr, QObject* parent = nullptr);
    ~MainUIBackend() override;

    // Navigation — lives on this class.
    int currentActiveSectionIndex() const;
    QVariantList sections() const;

    // Delegations to UIPluginManager.
    QVariantList uiModules() const;
    QVariantList launcherApps() const;
    QString      currentVisibleApp() const;
    QStringList  loadingModules() const;

    // Composed from multiple managers.
    QVariantList coreModules() const;

    // Build info accessors (see Q_PROPERTY declarations above).
    QString buildVersion() const;
    bool isPortableBuild() const;
    QVariantList buildCommits() const;

    // Accessors for C++ coordination code (MdiView etc.) that needs a handle
    // to the managers directly. QML goes through the delegating slots/signals.
    CoreModuleManager* coreModuleManager() const { return m_coreModuleManager; }
    UIPluginManager*   uiPluginManager()   const { return m_uiPluginManager; }
    PackageCoordinator*    packageCoordinator()    const { return m_packageCoordinator; }

public slots:
    // Navigation
    void setCurrentActiveSectionIndex(int index);

    // UI Module operations — delegated to UIPluginManager.
    void loadUiModule(const QString& moduleName);
    void unloadUiModule(const QString& moduleName);
    void activateApp(const QString& appName);

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

    // Core Module operations — routing rule: cascade-aware (load/unload)
    // goes through UIPluginManager so it can run the pre-flight dependent
    // check. Pure introspection (refresh, getMethods, callMethod) goes
    // directly to CoreModuleManager.
    void loadCoreModule(const QString& moduleName);
    void unloadCoreModule(const QString& moduleName);
    Q_INVOKABLE void refreshCoreModules();
    Q_INVOKABLE QString getCoreModuleMethods(const QString& moduleName);
    Q_INVOKABLE QString callCoreModuleMethod(const QString& moduleName, const QString& methodName, const QString& argsJson);

    // UI Modules refresh — forwarded to UIPluginManager which in turn asks
    // PackageCoordinator to rescan the catalog (PackageCoordinator owns the IPC).
    Q_INVOKABLE void refreshUiModules();

    // App Launcher operations — delegated to UIPluginManager.
    void onAppLauncherClicked(const QString& appName);
    void onPluginWindowClosed(const QString& pluginName);
    void setCurrentVisibleApp(const QString& pluginName);

signals:
    void currentActiveSectionIndexChanged();
    void uiModulesChanged();
    void coreModulesChanged();
    void launcherAppsChanged();
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

private:
    void initializeSections();

    // Navigation state — the only state this facade class holds.
    int m_currentActiveSectionIndex;
    QVariantList m_sections;

    // LogosAPI — shared with all three managers.
    LogosAPI* m_logosAPI;
    bool m_ownsLogosAPI;

    // Owned children (parent=this). Order matters: coreModuleManager first,
    // uiPluginManager second, packageCoordinator third. See class comment for
    // lifetime reasoning.
    CoreModuleManager* m_coreModuleManager;
    UIPluginManager*   m_uiPluginManager;
    PackageCoordinator*    m_packageCoordinator;
};
