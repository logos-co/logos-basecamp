#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <QHash>
#include <QMap>
#include <QPointer>
#include <QSet>
#include "logos_api.h"
#include "logos_api_client.h"
#include "IComponent.h"

class QQuickWidget;
class QTimer;
class PluginLoader;
class ViewModuleHost;
class CoreModuleManager;
class PackageCoordinator;
class IntentBridgeAdapter;
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

    // Forwarded to PluginLoader, which attaches each ui_qml app's bridge as it
    // loads. Pass-through, so this class never has to know what an intent is.
    void setIntentAdapter(IntentBridgeAdapter* adapter);

    // Unmounts every in-process UI plugin widget. Must run WHILE the shell's
    // widget tree is still alive — the widgets are docked inside it — which is
    // why Window drives this explicitly before destroying the shell rather
    // than leaving it to ~UIPluginManager. Idempotent.
    void shutdown();

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
    //
    // Usually completes before it returns. The exception is an in-process
    // (type: ui) plugin that answers Asynchronous to the aboutToUnload() hook:
    // that buys a bounded grace period during which the widget stays up and
    // the destruction runs from a continuation instead. Callers that need the
    // widget gone before they proceed cannot get that guarantee from this
    // function — see the comment on the definition for why blocking here is
    // not an option. Idempotence holds either way: a second call while a
    // deferral is in flight neither starts a second teardown nor tears down
    // underneath the one already running.
    void teardownUiPluginWidget(const QString& moduleName);

    // Resolve an installed UI plugin's icon from its manifest entry
    //   forWidgetIcon=false → "file://…" (the form QML's Image wants)
    //   forWidgetIcon=true  → raw "qrc:…" path (for QWidget::setWindowIcon)
    QString pluginIconUrl(const QString& moduleName, bool forWidgetIcon = false) const;

    // Manifest schema version of an installed UI plugin. Feeds the full-bleed
    // icon gate on both the sidebar and App Manager paths.
    QString pluginManifestVersion(const QString& moduleName) const;

    // By value, not by reference: the registry rebuilds by clear-and-refill, so
    // a reference held across a refresh is a use-after-free waiting to happen.
    QMap<QString, QVariantMap> uiPluginMetadataSnapshot() const;

    // Mounted in-process right now — distinct from "installed" and from "its
    // core module is loaded". An intent provider needs a live view.
    bool isUiAppLoaded(const QString& moduleName) const;

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
    // UI plugin whose core dependencies don't let it load; unloadCascade
    // fires when they try to unload a module other running things depend on.
    //
    // `blockers` is one map per blocking dependency (see
    // PackageCoordinator::blockingDepsOf); `summary` is one word for the set
    // ("absent" | "mismatch" | "signer" | "mixed"). Both, because the dialog
    // says a different sentence per kind and a bare name list cannot tell an
    // absent dependency from an installed one at the wrong version.
    void missingDepsPopupRequested(const QString& name,
                                   const QVariantList& blockers,
                                   const QString& summary);
    void unloadCascadeConfirmationRequested(const QString& name,
                                            const QStringList& loadedDependents);

    // Re-emitted from onPluginLoadFailed so MainUIBackend can forward a load
    // failure to the QML overlay instead of only logging it.
    void pluginLoadFailedNotice(const QString& name, const QString& error);


    // Failures filtered out of package_manager_ui's installationProgressUpdated.
    void packageInstallFailedNotice(const QString& packageName, const QString& errorMessage);

    // Plugin-window coordination — consumed by WorkspaceArea via
    // MainUIBackend's forwarders.
    void pluginWindowRequested(QWidget* widget, const QString& title);
    void pluginWindowRemoveRequested(QWidget* widget);

    // Presentation seam — see IShellHost::onPresentAppRequested for why the
    // shell, not this class, decides what "the front" means.
    void presentAppRequested(QWidget* widget);

    // The view finished loading and can receive. IntentBroker's activation
    // queue drains on this.
    void appReady(const QString& name);

    // The UI-plugin metadata cache was refilled — installs, uninstalls and
    // catalog refreshes all land here. IntentRegistry rebuilds on it.
    void uiPluginMetadataChanged();

private slots:
    void onPluginLoaded(const QString& name, QWidget* widget,
                        IComponent* component, UIPluginType type,
                        ViewModuleHost* viewHost);
    void onPluginLoadFailed(const QString& name, const QString& error);

    void onPmuiInstallProgress(int progressType, const QString& packageName,
                               int completed, int total, bool success,
                               const QString& error);

    // Consume the uiPluginsFetched signal from PackageCoordinator. Replaces the
    // old in-class getInstalledUiPluginsAsync call — PackageCoordinator owns the
    // IPC cadence now; this class just caches the UI-plugin-shaped subset
    // needed for widget loading.
    void onUiPluginsFetched(const QVariantList& uiPlugins);

    // Landing slot for a deferred-teardown plugin's unloadFinished() signal.
    // Reached through the string-based connect() in beginDeferredTeardown --
    // that overload needs a real slot on the receiving side, which a lambda is
    // not -- so the deferral it belongs to is found by sender().
    void onUiPluginUnloadFinished();

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

    // Force-reload a currently-loaded plugin's widget windowIcon from
    // disk, bypassing Qt's path-keyed pixmap cache
    void reloadLoadedPluginIcon(const QString& name, QWidget* widget) const;
    void loadLegacyUiModule(const QString& moduleName);
    QString resolveQmlViewPath(const QVariantMap& meta) const;
    QString getPluginPath(const QString& name) const;
    QString getPluginType(const QString& name) const;
    bool isQmlPlugin(const QString& name) const;
    bool hasBackendPlugin(const QString& name) const;

    // Cascade helpers
    QStringList loadedCoreModules() const;
    QStringList loadedDependentsOf(const QString& name) const;

    // Synchronous unload implementation — called directly from the shutdown
    // path and from the QueuedConnection lambda in unloadUiModule. Never call
    // this from a live QML signal handler; use unloadUiModule() instead.
    void unloadUiModuleImpl(const QString& moduleName);

    // --- aboutToUnload() teardown hook -------------------------------------

    // The in-process QObject that could carry the aboutToUnload() hook for
    // `moduleName`, or nullptr when there is none. Only legacy (type: ui)
    // plugins have one; see the definition for why ui_qml structurally
    // cannot.
    QObject* unloadHookTarget(const QString& moduleName) const;

    // One entry per teardown currently waiting on a plugin's unloadFinished().
    // Membership is the "a deferral is in flight for this name" flag, and
    // removing an entry is the one-shot guard that keeps the continuation from
    // running twice.
    struct DeferredTeardown {
        // The widget observed when the deferral was armed, used purely as a
        // staleness token: if the map no longer maps `moduleName` to this
        // exact widget when the continuation runs, something else already
        // tore the module down (or reloaded it) and the continuation must
        // not touch it. Never dereferenced — QPointer so a destroyed widget
        // reads as null rather than as a stale address that could compare
        // equal to a freshly allocated one.
        QPointer<QWidget>       widget;
        QObject*                plugin = nullptr;   // sender() we match on
        QTimer*                 deadline = nullptr; // owned (parent=this)
        QMetaObject::Connection finished;
        QElapsedTimer           elapsed;
    };
    QHash<QString, DeferredTeardown> m_deferredTeardowns;

    // Arm the unloadFinished()/deadline race for a plugin that answered
    // Asynchronous. Returns false if the deferral could not be set up, in
    // which case the caller must tear down immediately.
    bool beginDeferredTeardown(const QString& moduleName, QObject* plugin);

    // Remove and disarm the deferral for `moduleName`, returning what it held
    // (`plugin == nullptr` when there was none).
    DeferredTeardown takeDeferredTeardown(const QString& moduleName);

    // Continuation for a deferral, entered exactly once — from
    // onUiPluginUnloadFinished (finishedInTime=true) or from the deadline
    // timer (false). Whichever arrives first disarms the other.
    void resumeDeferredTeardown(const QString& moduleName, bool finishedInTime);

    // The destruction half of teardownUiPluginWidget: everything that runs
    // once the plugin has had its say. Called directly on the synchronous
    // path and from resumeDeferredTeardown on the asynchronous one.
    void teardownUiPluginWidgetNow(const QString& moduleName);

    // Wiring
    LogosAPI*          m_logosAPI;          // not owned
    CoreModuleManager* m_coreModuleManager; // not owned (sibling Qt child)
    PackageCoordinator*    m_packageCoordinator;    // not owned (sibling Qt child); nullable until setPackageCoordinator

    // Load parked on dependencyDataReadyChanged; empty when idle. Mirrors
    // PackageCoordinator::uninstallApp's deferral — last click wins.
    QString                 m_pendingGatedLoadName;
    QMetaObject::Connection m_pendingGatedLoadConn;
    PluginLoader*      m_pluginLoader;      // owned (parent=this)

    // Loaded-plugin state
    QMap<QString, IComponent*>   m_loadedUiModules;
    // QPointer, not raw: these widgets are docked inside the shell, so the
    // shell's Qt parent can destroy them without going through unloadUiModule,
    // leaving every read below dangling. Window's ordered teardown calls
    // beginShutdown() first; QPointer is the backstop if some path does not.
    QMap<QString, QPointer<QWidget>>      m_uiModuleWidgets;
    QMap<QString, QPointer<QQuickWidget>> m_qmlPluginWidgets;
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
