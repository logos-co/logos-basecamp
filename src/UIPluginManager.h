#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <QMap>
#include <QSet>
#include "logos_api.h"
#include "logos_api_client.h"
#include "IComponent.h"

class QQuickWidget;
class PluginLoader;
class ViewModuleHost;
class CoreModuleManager;
class PackageCoordinator;
enum class UIPluginType;

// UIPluginManager — owns UI plugin widget lifecycle in this process.
//
// Scope:
//   * Loaded UI widgets (legacy ui-plugin + ui_qml) and view-module hosts
//   * UI-plugin metadata cache (m_uiPluginMetadata) fed by PackageCoordinator's
//     uiPluginsFetched signal — consumed here for load dispatch (type, path,
//     view entry point, icon).
//   * App launcher visibility bookkeeping (m_loadedApps, m_currentVisibleApp)
//   * Local unload-cascade confirmation (no package_manager involvement —
//     just a "you're about to orphan dependents of this running module" gate).
//
// What it does NOT do:
//   * Talk to the `package_manager` LogosAPI module. All install/uninstall/
//     upgrade IPC, the install-confirmation dialog, the uninstall cascade
//     dialog, and the installType / missing-deps / dependents caches live
//     in PackageCoordinator. This class queries PackageCoordinator via the pointer
//     injected by setPackageCoordinator for any package-state it needs.
//   * Touch the logos_core_* C API directly — every core-plugin load/unload/
//     cascade call goes through m_coreModuleManager (see CoreModuleManager.h).
//   * Navigation, sections, or QML wiring — that's MainUIBackend's facade job.
//
// Why its own class: the old PluginManager mixed UI-widget bookkeeping with
// every package_manager IPC call, leaving cascade paths tangled with widget
// ownership. Splitting lets this class reason about one concern — UI plugin
// widgets and their load state — while PackageCoordinator handles the catalog/
// install/uninstall/upgrade surface against the module.
class UIPluginManager : public QObject {
    Q_OBJECT

public:
    // coreModuleManager is NOT owned — it's a sibling Qt child of the same
    // MainUIBackend. Construction order guarantees it outlives
    // UIPluginManager via Qt's reverse-order child destruction
    // (CoreModuleManager constructed first, destroyed last).
    //
    // packageCoordinator is set later via setPackageCoordinator — it's a sibling
    // child constructed AFTER UIPluginManager (it depends on this class for
    // cascade cooperation), so we can't take it in the ctor. Keep every read
    // of m_packageCoordinator guarded against null.
    explicit UIPluginManager(LogosAPI* logosAPI,
                             CoreModuleManager* coreModuleManager,
                             QObject* parent = nullptr);
    ~UIPluginManager() override;

    // Setter injection for the sibling PackageCoordinator. Called from
    // MainUIBackend right after PackageCoordinator is constructed. Also wires the
    // uiPluginsFetched signal so catalog refreshes flow into
    // m_uiPluginMetadata without this class having to talk to the module.
    void setPackageCoordinator(PackageCoordinator* packageCoordinator);

    // QML-bound getters (surfaced via MainUIBackend's Q_PROPERTYs).
    QVariantList uiModules() const;
    QVariantList launcherApps() const;
    QString      currentVisibleApp() const;
    QStringList  loadingModules() const;

    // Cross-class helpers used by PackageCoordinator during cascade work.

    // Filter `moduleNames` down to the subset currently loaded — either as a
    // running core plugin (tracked by liblogos) or as a UI-plugin widget
    // mounted in this process. Used by PackageCoordinator's cascade paths to
    // compute the "loaded dependents" list for the confirmation dialog.
    QStringList intersectWithLoaded(const QStringList& moduleNames) const;

    // Idempotent widget teardown. PackageCoordinator calls this during the
    // uninstall/upgrade cascade so UI-plugin dependents whose backing core
    // module just died don't outlive it as orphaned widgets.
    void teardownUiPluginWidget(const QString& moduleName);

public slots:
    // UI module lifecycle
    void loadUiModule(const QString& moduleName);
    void unloadUiModule(const QString& moduleName);
    void activateApp(const QString& appName);

    // Local unload-cascade confirmation flow, called from QML dialogs.
    Q_INVOKABLE void confirmUnloadCascade(const QString& moduleName);
    Q_INVOKABLE void cancelUnloadCascade(const QString& moduleName);

    // Core module lifecycle (cascade-aware; delegates C API calls to
    // CoreModuleManager).
    void loadCoreModule(const QString& moduleName);
    void unloadCoreModule(const QString& moduleName);

    // Full UI-metadata rescan — mirror of CoreModuleManager::refresh for the
    // UI Modules tab's Reload button. Forwards to PackageCoordinator (which owns
    // the refresh cadence).
    Q_INVOKABLE void refreshUiModules();

    // App launcher
    void onAppLauncherClicked(const QString& appName);
    void onPluginWindowClosed(const QString& pluginName);
    void setCurrentVisibleApp(const QString& pluginName);

signals:
    // QML-visible property-change signals. MainUIBackend re-emits each into
    // its own matching signal via a signal-to-signal connect.
    void uiModulesChanged();
    void launcherAppsChanged();
    void loadingModulesChanged();
    void currentVisibleAppChanged();
    void navigateToApps();

    // Core-modules state can flip as a side effect of cascade paths — QML
    // binds to MainUIBackend::coreModulesChanged which aggregates this
    // plus CoreModuleManager::coreModulesChanged.
    void coreModulesChanged();

    // Dependency-aware UX. missingDepsPopup fires when the user clicks a
    // UI plugin that can't load because its core deps aren't installed;
    // unloadCascade fires when they try to unload a module other running
    // things depend on.
    void missingDepsPopupRequested(const QString& name, const QStringList& missing);
    void unloadCascadeConfirmationRequested(const QString& name,
                                            const QStringList& loadedDependents);

    // MDI coordination — consumed by C++ MdiView via MainUIBackend's forwarders.
    void pluginWindowRequested(QWidget* widget, const QString& title);
    void pluginWindowRemoveRequested(QWidget* widget);
    void pluginWindowActivateRequested(QWidget* widget);

private slots:
    void onPluginLoaded(const QString& name, QWidget* widget,
                        IComponent* component, UIPluginType type,
                        ViewModuleHost* viewHost);
    void onPluginLoadFailed(const QString& name, const QString& error);

    // Consume the uiPluginsFetched signal from PackageCoordinator. Replaces the
    // old in-class getInstalledUiPluginsAsync call — PackageCoordinator owns the
    // IPC cadence now; this class just caches the UI-plugin-shaped subset
    // needed for widget loading.
    void onUiPluginsFetched(const QVariantList& uiPlugins);

private:
    // Local unload-cascade pending slot. Set when unloadUiModule /
    // unloadCoreModule detects a loaded dependent and asks the user to
    // confirm the cascade. No package_manager involvement — this is purely
    // about which plugins are running in this process.
    struct PendingUnload {
        bool    active = false;
        QString name;
    };

    // UI-plugin helpers. All read from m_uiPluginMetadata.
    QStringList findAvailableUiPlugins() const;
    void loadLegacyUiModule(const QString& moduleName);
    QString resolveQmlViewPath(const QVariantMap& meta) const;
    QString getPluginPath(const QString& name) const;
    QString getPluginType(const QString& name) const;
    bool isQmlPlugin(const QString& name) const;
    bool hasBackendPlugin(const QString& name) const;
    QString getPluginIconPath(const QString& pluginName, bool forWidgetIcon = false) const;

    // Cascade helpers
    QStringList loadedCoreModules() const;
    QStringList loadedDependentsOf(const QString& name) const;

    // Synchronous unload implementation — called directly from the shutdown
    // path and from the QueuedConnection lambda in unloadUiModule. Never call
    // this from a live QML signal handler; use unloadUiModule() instead.
    void unloadUiModuleImpl(const QString& moduleName);

    // Wiring
    LogosAPI*          m_logosAPI;          // not owned
    CoreModuleManager* m_coreModuleManager; // not owned (sibling Qt child)
    PackageCoordinator*    m_packageCoordinator;    // not owned (sibling Qt child); nullable until setPackageCoordinator
    PluginLoader*      m_pluginLoader;      // owned (parent=this)

    // Loaded-plugin state
    QMap<QString, IComponent*>   m_loadedUiModules;
    QMap<QString, QWidget*>      m_uiModuleWidgets;
    QMap<QString, QQuickWidget*> m_qmlPluginWidgets;
    QMap<QString, ViewModuleHost*> m_viewModuleHosts;

    // App launcher
    QSet<QString> m_loadedApps;
    QString       m_currentVisibleApp;

    // Cache of UI plugin name → metadata, fed by PackageCoordinator's
    // uiPluginsFetched signal. Used to dispatch loads (type, path, view,
    // icon). Not exposed outside this class — PackageCoordinator and QML query
    // the package-state caches on PackageCoordinator directly.
    QMap<QString, QVariantMap> m_uiPluginMetadata;

    // Local unload-cascade pending slot.
    PendingUnload m_pendingUnload;

    // Set by the destructor (and only the destructor) to tell
    // unloadUiModule/unloadCoreModule to bypass the cascade-confirmation
    // fast-path and tear down directly. Without this, the first loaded
    // module with loaded dependents would early-return to emit
    // unloadCascadeConfirmationRequested (into a tearing-down QML
    // that can never call confirmUnloadCascade) and its widget/host
    // would leak. Defaults to false.
    bool m_shuttingDown = false;
};
