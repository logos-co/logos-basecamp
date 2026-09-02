#include "UIPluginManager.h"

#include "IntentBridgeAdapter.h"
#include "AppsModel.h"
#include "CoreModuleManager.h"
#include "PackageCoordinator.h"
#include "PluginLoader.h"
#include "utils/DependencyBlocker.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QQmlContext>
#include <QQuickWidget>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include "LogosQmlBridge.h"

#include <ViewModuleHost.h>

namespace {

// How long an in-process UI plugin that answers Asynchronous from
// aboutToUnload() gets before we tear its widget down anyway.
//
// Same 3000ms the out-of-process module host spends
// (logos-module-loader-qt/src/host/logos_host.cpp:230) so a module author has
// one number to reason about rather than two. It is not the same *kind* of
// budget, though: there it is carved out of the container's 5s SIGKILL window,
// and overrunning it gets the module killed. Nothing kills Basecamp at the end
// of this one. The bound is here so a plugin that answers Asynchronous and
// then never calls unloadFinished() cannot pin its widget on screen — and,
// through the uninstall cascade that calls us, stall a package operation —
// indefinitely.
constexpr int kUnloadGraceMs = 3000;

} // namespace

UIPluginManager::UIPluginManager(LogosAPI* logosAPI,
                                 CoreModuleManager* coreModuleManager,
                                 QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
    , m_coreModuleManager(coreModuleManager)
    , m_packageCoordinator(nullptr)
    , m_pluginLoader(nullptr)
    , m_currentVisibleApp("")
{
    m_pluginLoader = new PluginLoader(m_logosAPI, m_coreModuleManager, this);
    connect(m_pluginLoader, &PluginLoader::pluginLoaded,
            this, &UIPluginManager::onPluginLoaded);
    connect(m_pluginLoader, &PluginLoader::pluginLoadFailed,
            this, &UIPluginManager::onPluginLoadFailed);
    connect(m_pluginLoader, &PluginLoader::loadingChanged,
            this, &UIPluginManager::loadingModulesChanged);
}

UIPluginManager::~UIPluginManager()
{
    // Safety net for the path that does not call shutdown() first. Idempotent:
    // shutdown() returns immediately if it has already run.
    shutdown();
}

void UIPluginManager::shutdown()
{
    // Idempotent — MainUIBackend::beginShutdown() normally gets here first,
    // while the shell's widget tree is still intact, and the destructor then
    // finds nothing left to do.
    if (m_shuttingDown) {
        return;
    }

    // Tell unloadUiModule/unloadCoreModule to bypass the cascade-
    // confirmation fast-path — there's no user to confirm and no live
    // QML layer to drive the dialog. Otherwise the first loaded module
    // with loaded dependents would early-return at the
    // unloadCascadeConfirmationRequested emit, leaving its widget/host
    // leaked AND setting m_pendingUnload.active=true, which lets the
    // subsequent iterations' cascade checks succeed vacuously without
    // ever returning to finish the aborted one.
    m_shuttingDown = true;

    // Tear down every in-process UI plugin widget before our members
    // disappear. Snapshot the keys from both maps since both legacy and
    // ui_qml plugins need to go.
    QStringList moduleNames = m_loadedUiModules.keys();
    for (const QString& name : m_qmlPluginWidgets.keys()) {
        if (!moduleNames.contains(name)) {
            moduleNames.append(name);
        }
    }

    for (const QString& name : moduleNames) {
        unloadUiModule(name);
    }
}

void UIPluginManager::setPackageCoordinator(PackageCoordinator* packageCoordinator)
{
    if (m_packageCoordinator == packageCoordinator) return;
    m_packageCoordinator = packageCoordinator;
    if (!m_packageCoordinator) return;

    // Wire the catalog-refresh signal — PackageCoordinator owns the IPC cadence
    // (and the event subscriptions that trigger refreshes on install /
    // uninstall); we just consume the resulting UI-plugin list to keep our
    // load-dispatch cache current.
    connect(m_packageCoordinator, &PackageCoordinator::uiPluginsFetched,
            this, &UIPluginManager::onUiPluginsFetched);

    // When PackageCoordinator's dep-info refresh completes, our uiModules() /
    // launcherApps() builders need to re-emit so QML binds to the new
    // installType / missing-deps values.
    connect(m_packageCoordinator, &PackageCoordinator::uiModulesChanged,
            this, &UIPluginManager::uiModulesChanged);
    connect(m_packageCoordinator, &PackageCoordinator::launcherAppsChanged,
            this, &UIPluginManager::launcherAppsChanged);
    connect(m_packageCoordinator, &PackageCoordinator::coreModulesChanged,
            this, &UIPluginManager::coreModulesChanged);
}

void UIPluginManager::onUiPluginsFetched(const QVariantList& uiPlugins)
{
    m_uiPluginMetadata.clear();
    for (const QVariant& item : uiPlugins) {
        QVariantMap pluginInfo = item.toMap();
        QString name = pluginInfo.value("name").toString();
        if (name.isEmpty()) continue;

        // ui_qml requires "view" (the QML entry point); "main" is optional.
        // Other types require "mainFilePath" (the backend lib).
        const QString type = pluginInfo.value("type").toString();
        if (type == QStringLiteral("ui_qml")) {
            if (pluginInfo.value("view").toString().isEmpty()) continue;
        } else {
            if (pluginInfo.value("mainFilePath").toString().isEmpty()) continue;
        }
        m_uiPluginMetadata[name] = pluginInfo;
    }

    for (auto it = m_qmlPluginWidgets.cbegin(); it != m_qmlPluginWidgets.cend(); ++it)
        reloadLoadedPluginIcon(it.key(), it.value());
    for (auto it = m_uiModuleWidgets.cbegin(); it != m_uiModuleWidgets.cend(); ++it)
        reloadLoadedPluginIcon(it.key(), it.value());

    // Immediate emit — the package-state caches (installType, missingDeps)
    // on PackageCoordinator may still be refreshing, but the list of UI plugins
    // has changed now and QML should reflect that.
    emit uiModulesChanged();
    emit launcherAppsChanged();
    emit uiPluginMetadataChanged();
}

void UIPluginManager::reloadLoadedPluginIcon(const QString& name, QWidget* widget) const
{
    if (!widget) return;
    const QString iconPath = pluginIconUrl(name, /*forWidgetIcon=*/true);
    if (iconPath.isEmpty()) return;
    // qrc-scheme icons are compiled in and can't have changed on disk.
    if (iconPath.startsWith(QLatin1String(":/"))) return;

    QFile f(iconPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    QPixmap pm;
    if (!pm.loadFromData(f.readAll())) return;

    widget->setWindowIcon(QIcon(pm));
}

QVariantList UIPluginManager::uiModules() const
{
    QVariantList modules;
    QStringList availablePlugins = findAvailableUiPlugins();

    for (const QString& pluginName : availablePlugins) {
        const QVariantMap& meta = m_uiPluginMetadata.value(pluginName);
        const bool isInstalled = meta.value("isInstalled", true).toBool();

        QVariantMap module;
        module["name"] = pluginName;
        module["isInstalled"] = isInstalled;
        module["isLoaded"] = m_loadedUiModules.contains(pluginName) || m_qmlPluginWidgets.contains(pluginName);
        module["isMainUi"] = (pluginName == "main_ui");
        module["iconPath"] = pluginIconUrl(pluginName);
        module["displayName"] = meta.value("displayName");
        module["description"] = meta.value("description");
        module["repositoryUrl"] = meta.value("repositoryUrl");
        module["version"] = meta.value("version");

        // Dependency-aware fields come from PackageCoordinator. If it hasn't
        // finished its async refresh yet, the accessors return empty values
        // which QML treats as "unknown — render safe defaults" (no red-cross,
        // no Uninstall button).
        const QStringList missing = (isInstalled && m_packageCoordinator)
            ? m_packageCoordinator->missingDepsOf(pluginName)
            : QStringList{};
        module["installType"] = (isInstalled && m_packageCoordinator)
            ? m_packageCoordinator->installType(pluginName)
            : QString(); // "" | "embedded" | "user"
        module["hasMissingDeps"] = !missing.isEmpty();
        module["missingDeps"] = missing;
        // Which KIND, so the row badge can say "Version conflict" rather than
        // "Missing deps" about a dependency that is very much installed.
        module["depBlockKind"] = (isInstalled && m_packageCoordinator)
            ? logos::summariseDependencyBlockers(
                  m_packageCoordinator->blockingDepsOf(pluginName))
            : QString();

        modules.append(module);
    }

    return modules;
}

void UIPluginManager::loadUiModule(const QString& moduleName)
{
    qDebug() << "Loading UI module:" << moduleName;

    if (m_loadedUiModules.contains(moduleName) || m_qmlPluginWidgets.contains(moduleName)) {
        qDebug() << "Module" << moduleName << "is already loaded";
        activateApp(moduleName);
        return;
    }

    // The caches answer "empty" both for "nothing blocks this" and for "not
    // asked yet", and the tiles are published before the dependency fan-out
    // runs (PackageCoordinator.cpp: uiPluginsFetched precedes
    // refreshDependencyInfo). Reading through that window fails OPEN, so park
    // the load until the data exists. Same shape as
    // PackageCoordinator::uninstallApp; last click wins.
    if (m_packageCoordinator && !m_packageCoordinator->dependencyDataReady()) {
        qDebug() << "UI module" << moduleName
                 << "deferred: dependency data not ready";
        QObject::disconnect(m_pendingGatedLoadConn);
        m_pendingGatedLoadName = moduleName;
        QPointer<UIPluginManager> self(this);
        m_pendingGatedLoadConn = connect(
            m_packageCoordinator, &PackageCoordinator::dependencyDataReadyChanged,
            this, [self, moduleName]() {
                if (!self) return;
                if (self->m_pendingGatedLoadName != moduleName) return;   // superseded
                self->m_pendingGatedLoadName.clear();
                QObject::disconnect(self->m_pendingGatedLoadConn);
                self->loadUiModule(moduleName);
            });
        return;
    }

    // Gate on core dependencies the resolver won't accept, so the user gets a
    // popup instead of a cryptic "plugin load failed". An INSTALLED dependency
    // outside the declared range refuses the load exactly as an absent one
    // does, so the popup must say which: "install it" and "you have the wrong
    // version" are different instructions.
    const QVariantList blockers = m_packageCoordinator
        ? m_packageCoordinator->blockingDepsOf(moduleName)
        : QVariantList{};
    if (!blockers.isEmpty()) {
        const QString summary = logos::summariseDependencyBlockers(blockers);
        qDebug() << "UI module" << moduleName << "blocked by deps (" << summary
                 << "):" << blockers;
        emit missingDepsPopupRequested(moduleName, blockers, summary);
        return;
    }

    if (isQmlPlugin(moduleName)) {
        const QVariantMap& meta = m_uiPluginMetadata.value(moduleName);

        PluginLoadRequest request;
        request.name = moduleName;
        request.type = UIPluginType::UiQml;
        request.installDir = meta.value("installDir").toString();
        request.qmlViewPath = resolveQmlViewPath(meta);
        request.iconPath = pluginIconUrl(moduleName, true);
        if (hasBackendPlugin(moduleName))
            request.mainFilePath = meta.value("mainFilePath").toString();
        request.coreDependencies = meta.value("dependencies").toList();

        m_pluginLoader->load(request);
        return;
    }

    loadLegacyUiModule(moduleName);
}

void UIPluginManager::onPluginLoaded(const QString& name, QWidget* widget,
                                     IComponent* component, UIPluginType type,
                                     ViewModuleHost* viewHost)
{
    if (component)
        m_loadedUiModules[name] = component;
    if (type != UIPluginType::Legacy)
        m_qmlPluginWidgets[name] = qobject_cast<QQuickWidget*>(widget);
    if (viewHost)
        m_viewModuleHosts[name] = viewHost;
    m_uiModuleWidgets[name] = widget;
    m_loadedApps.insert(name);

    // TODO - check if this generic enough for extedning different capabilities 
    // from basecamp to other apps
    // For ui_qml view modules, wire up any signals we care about from the
    // QtRO replica before QML sees the widget. The replica is already
    // created inside LogosQmlBridge at this point (setViewModuleSocket was
    // called in PluginLoader::onHostReady before pluginLoaded was emitted);
    // it may not yet be Valid, but Qt signal/slot connections work regardless
    // of replica state — the connection will fire when the source emits.
    if (type == UIPluginType::UiQml) {
        QQuickWidget* qw = m_qmlPluginWidgets.value(name);
        if (qw) {
            auto* bridge = qobject_cast<LogosQmlBridge*>(
                qw->rootContext()->contextProperty(QStringLiteral("logos"))
                    .value<QObject*>());
            if (bridge) {
                if (name == QStringLiteral("package_manager_ui")) {
                    QObject* replica = bridge->module(name);
                    if (replica) {
                        // String-based connects resolve against the replica's
                        // metaobject at runtime — a signature drift in the PMU
                        // .rep would fail silently, so guard each result.
                        if (!connect(replica,
                                     SIGNAL(installationProgressUpdated(int,QString,int,int,bool,QString)),
                                     this,
                                     SLOT(onPmuiInstallProgress(int,QString,int,int,bool,QString)))) {
                            qCritical() << "package_manager_ui replica signal"
                                        << "installationProgressUpdated(...) not found"
                                        << "- signature drift vs package_manager_ui.rep?"
                                        << "Install failures will NOT surface in the UI.";
                        }
                    }
                }
            }
        }
    }

    emit uiModulesChanged();
    emit launcherAppsChanged();
    emit pluginWindowRequested(widget, name);
    emit navigateToApps();

    // Last, and after the window exists: a broker draining its activation queue
    // dispatches into the view immediately, so it must already be mounted.
    emit appReady(name);

    qDebug() << "Successfully loaded UI module:" << name;
}

void UIPluginManager::onPluginLoadFailed(const QString& name, const QString& error)
{
    qWarning() << "Failed to load UI module" << name << ":" << error;
    // Surface to the user, not just the log (forwarded to the QML overlay).
    emit pluginLoadFailedNotice(name, error);
}

void UIPluginManager::onPmuiInstallProgress(int progressType, const QString& packageName,
                                            int completed, int total, bool success,
                                            const QString& error)
{
    Q_UNUSED(progressType);
    Q_UNUSED(completed);
    Q_UNUSED(total);
    // `success` discriminates, not the error string — progress emits reuse it
    // for non-error status text ("Upgrading X…").
    if (success) {
        return;
    }
    qWarning() << "package_manager_ui install failure for" << packageName << ":" << error;
    emit packageInstallFailedNotice(
        packageName,
        error.isEmpty() ? QStringLiteral("The package manager reported an "
                                         "unspecified installation failure.")
                        : error);
}

QStringList UIPluginManager::loadingModules() const
{
    return m_pluginLoader->loadingPlugins();
}

void UIPluginManager::unloadUiModule(const QString& moduleName)
{
    if (m_shuttingDown) {
        // Shutdown path: run synchronously — no QML signal handler is on
        // the stack and we need the teardown to complete before Qt child
        // destruction continues. The cascade guard below skips when
        // m_shuttingDown is true, so this goes straight to teardown.
        unloadUiModuleImpl(moduleName);
        return;
    }

    // Normal path: defer the whole body — same rationale as loadCoreModule /
    // unloadCoreModule. This slot is invoked from a QML Button.onClicked handler
    // inside a view delegate (e.g. the Apps Inspector table's "Unload" button).
    // Emitting uiModulesChanged() synchronously causes the model reset to fire,
    // which calls clear() → setParentItem(nullptr) on every delegate, including
    // the button that was just clicked. QQuickItemPrivate::derefWindow then
    // crashes trying to walk that button's child tree while the window pointer
    // on one of its children is already null.
    // Deferring via QueuedConnection lets the click handler fully unwind first;
    // by the time the lambda runs the Repeater delegate tree is stable again.
    QMetaObject::invokeMethod(this, [this, moduleName]{
        unloadUiModuleImpl(moduleName);
    }, Qt::QueuedConnection);
}

void UIPluginManager::unloadUiModuleImpl(const QString& moduleName)
{
    qDebug() << "Unloading UI module:" << moduleName;

    bool isQml = m_qmlPluginWidgets.contains(moduleName);
    bool isCpp = m_loadedUiModules.contains(moduleName);

    if (!isQml && !isCpp) {
        qDebug() << "Module" << moduleName << "is not loaded";
        return;
    }

    // Cascade check: if this UI module exposes a core plugin that other
    // loaded plugins depend on, unloading it would silently strand them.
    // Emit the confirmation signal and wait for confirmUnloadCascade() or
    // cancelUnloadCascade(). We only intercept when there's actually a
    // dependent loaded — the common case (leaf plugin) skips the dialog.
    //
    // Guard against re-entering the flow if we're already in a pending
    // cascade for a *different* module; otherwise we could lose state.
    // Skip the cascade entirely during destruction — the QML that would
    // drive the dialog is gone and we need to actually tear down, not
    // await a confirmation that can never arrive.
    //
    // Scope the skip to a re-entry of the SAME module (the confirm path clears
    // m_pendingUnload before re-calling); a cascade pending for another module
    // must not suppress this one's dependent check.
    const bool reentryForSameModule =
        m_pendingUnload.active && m_pendingUnload.name == moduleName;
    if (!m_shuttingDown && !reentryForSameModule) {
        const QStringList loadedDeps = loadedDependentsOf(moduleName);
        if (!loadedDeps.isEmpty()) {
            m_pendingUnload = {true, moduleName};
            qDebug() << "Unload cascade needed for" << moduleName << "dependents:" << loadedDeps;
            emit unloadCascadeConfirmationRequested(moduleName, loadedDeps);
            return;
        }
    }

    QWidget* widget = m_uiModuleWidgets.value(moduleName);
    IComponent* component = m_loadedUiModules.value(moduleName);

    if (widget) {
        emit pluginWindowRemoveRequested(widget);
    }

    if (component && widget) {
        component->destroyWidget(widget);
    }

    if (isQml && widget) {
        widget->deleteLater();
    }

    // Stop view module host process if this was a view module
    if (m_viewModuleHosts.contains(moduleName)) {
        m_viewModuleHosts[moduleName]->stop();
        delete m_viewModuleHosts.take(moduleName);
    }

    m_loadedUiModules.remove(moduleName);
    m_uiModuleWidgets.remove(moduleName);
    m_qmlPluginWidgets.remove(moduleName);
    m_loadedApps.remove(moduleName);

    emit uiModulesChanged();
    emit launcherAppsChanged();

    if (m_currentVisibleApp == moduleName) {
        m_currentVisibleApp.clear();
        emit currentVisibleAppChanged();
    }

    qDebug() << "Successfully unloaded UI module:" << moduleName;
}

void UIPluginManager::activateApp(const QString& appName)
{
    QWidget* widget = m_uiModuleWidgets.value(appName);
    if (!widget) return;

    // Before the raise, so a synchronous receiver already sees the new app.
    setCurrentVisibleApp(appName);

    // No navigateToApps(): which section to land on is the shell's call, since
    // only it knows whether the widget was docked or hoisted into the stack.
    emit presentAppRequested(widget);
}


void UIPluginManager::setIntentAdapter(IntentBridgeAdapter* adapter)
{
    if (m_pluginLoader) m_pluginLoader->setIntentAdapter(adapter);
}

QMap<QString, QVariantMap> UIPluginManager::uiPluginMetadataSnapshot() const
{
    return m_uiPluginMetadata;
}

bool UIPluginManager::isUiAppLoaded(const QString& moduleName) const
{
    return m_uiModuleWidgets.contains(moduleName)
        || m_qmlPluginWidgets.contains(moduleName);
}

void UIPluginManager::setCurrentVisibleApp(const QString& pluginName)
{
    if (m_currentVisibleApp != pluginName) {
        m_currentVisibleApp = pluginName;
        emit currentVisibleAppChanged();
        emit launcherAppsChanged();
    }
}

QString UIPluginManager::currentVisibleApp() const
{
    return m_currentVisibleApp;
}

void UIPluginManager::loadCoreModule(const QString& moduleName)
{
    // Defer the ENTIRE body — not just the emit. Callers are typically
    // QML Button.onClicked handlers, and m_coreModuleManager->loadModule
    // ultimately calls logos_core_load_module_with_dependencies, which
    // internally spins a nested Qt event loop (via
    // QConnectedReplicaImplementation::waitForSource during the
    // informModuleToken round-trip, see liblogos_core). Running that
    // nested loop while a QML signal handler is still on the stack lets
    // Qt deliver a pending DeferredDelete for the firing Button/Repeater
    // delegate, and then the destructor trips "Object destroyed while
    // one of its QML signal handlers is in progress" → qFatal.
    // Queueing through the event loop lets the click handler unwind
    // first; the nested loop is then harmless.
    QMetaObject::invokeMethod(this, [this, moduleName]{
        qDebug() << "Loading core module:" << moduleName;

        bool success = m_coreModuleManager
                     ? m_coreModuleManager->loadModule(moduleName)
                     : false;

        if (success) {
            qDebug() << "Successfully loaded core module:" << moduleName;
            emit coreModulesChanged();
        } else {
            qDebug() << "Failed to load core module:" << moduleName;
        }
    }, Qt::QueuedConnection);
}

void UIPluginManager::unloadCoreModule(const QString& moduleName)
{
    // Shutdown path runs synchronously (no QML on the stack to worry about)
    // and must not defer — we need the teardown to happen before Qt child
    // destruction continues past our destructor body.
    if (m_shuttingDown) {
        qDebug() << "Unloading core module:" << moduleName;
        if (m_coreModuleManager) {
            bool success = m_coreModuleManager->unloadModule(moduleName);
            if (success) qDebug() << "Successfully unloaded core module:" << moduleName;
            else         qDebug() << "Failed to unload core module:" << moduleName;
        }
        return;
    }

    // Normal path: defer the whole body — same rationale as loadCoreModule.
    // m_coreModuleManager->unloadModule → logos_core_unload_module spins a
    // nested event loop inside the QRemoteObjects teardown handshake, and
    // this slot is typically invoked from a QML Button.onClicked. Running
    // the nested loop while the click handler is still on the stack is
    // what trips QQmlData::destroyed's "Object destroyed while one of its
    // QML signal handlers is in progress" qFatal.
    QMetaObject::invokeMethod(this, [this, moduleName]{
        qDebug() << "Unloading core module:" << moduleName;

        // Cascade check — mirror unloadUiModule. Without this, clicking
        // "Unload" on a core module with loaded dependents (core or UI)
        // silently orphans them. The confirmation dialog path is only
        // engaged when there's at least one loaded dependent, so leaf
        // unloads still take the fast path.
        //
        // Scope the re-entry guard to the SAME module (see unloadUiModule).
        const bool reentryForSameModule =
            m_pendingUnload.active && m_pendingUnload.name == moduleName;
        if (!reentryForSameModule) {
            const QStringList loadedDeps = loadedDependentsOf(moduleName);
            if (!loadedDeps.isEmpty()) {
                m_pendingUnload = {true, moduleName};
                qDebug() << "Unload cascade needed for core module" << moduleName
                         << "dependents:" << loadedDeps;
                emit unloadCascadeConfirmationRequested(moduleName, loadedDeps);
                return;
            }
        }

        bool success = m_coreModuleManager
                     ? m_coreModuleManager->unloadModule(moduleName)
                     : false;

        if (success) {
            qDebug() << "Successfully unloaded core module:" << moduleName;
            emit coreModulesChanged();
        } else {
            qDebug() << "Failed to unload core module:" << moduleName;
        }
    }, Qt::QueuedConnection);
}

void UIPluginManager::refreshUiModules()
{
    // The refresh cadence is owned by PackageCoordinator — it scans the disk
    // via the module and pushes results back to us via uiPluginsFetched.
    // refreshUiModules() is the user-triggered kick for that scan (wired to
    // the Reload button on the UI Modules tab).
    qDebug() << "Refreshing UI modules";
    if (m_packageCoordinator) {
        m_packageCoordinator->refresh();
    }
}

QVariantList UIPluginManager::launcherApps() const
{
    QVariantList apps;
    QStringList availablePlugins = findAvailableUiPlugins();

    for (const QString& pluginName : availablePlugins) {
        if (pluginName == "main_ui") {
            continue;
        }

        if (pluginName == "package_manager_ui") {
            continue;
        }

        QVariantMap app;
        app["name"] = pluginName;
        const QString metaDn =
            m_uiPluginMetadata.value(pluginName).value("displayName").toString();
        app["displayName"] = metaDn;
        app["isLoaded"] = m_loadedApps.contains(pluginName);
        app["iconPath"] = pluginIconUrl(pluginName);
        // Manifest >= 0.4.0 guarantees a validated 256x256 icon, so the
        // sidebar tile can render it edge-to-edge; older packages ship a
        // small glyph that must stay inset.
        app["supportsFullBleedIcon"] =
            AppsModel::supportsFullBleedIcon(pluginManifestVersion(pluginName));
        // Sidebar marker source, read directly by SidebarAppDelegate. The
        // blocker list is deliberately not shipped here: the sidebar draws
        // only an indicator, and the click-triggered popup fetches the detail
        // from PackageCoordinator::blockingDepsOf.
        const QStringList missing = m_packageCoordinator
            ? m_packageCoordinator->missingDepsOf(pluginName)
            : QStringList{};
        app["hasMissingDeps"] = !missing.isEmpty();
        // "" | "absent" | "mismatch" | "signer" | "mixed" — picks the
        // marker's shape.
        app["depBlockKind"] = m_packageCoordinator
            ? logos::summariseDependencyBlockers(
                  m_packageCoordinator->blockingDepsOf(pluginName))
            : QString();

        apps.append(app);
    }

    return apps;
}

void UIPluginManager::onAppLauncherClicked(const QString& appName)
{
    qDebug() << "App launcher clicked:" << appName;

    setCurrentVisibleApp(appName);
    if (m_loadedApps.contains(appName)) {
        activateApp(appName);
    } else {
        loadUiModule(appName);
    }
}

void UIPluginManager::confirmUnloadCascade(const QString& moduleName)
{
    if (!m_pendingUnload.active || m_pendingUnload.name != moduleName) {
        qWarning() << "confirmUnloadCascade for" << moduleName
                   << "but pending unload is" << m_pendingUnload.name;
        return;
    }
    // Clear pending synchronously so a second click on the dialog's Continue
    // (or a racing cancel) sees the slot as free. The actual unload work is
    // deferred below.
    m_pendingUnload = {};

    // Defer the cascade body — same rationale as loadCoreModule /
    // unloadCoreModule. confirmUnloadCascade is invoked from the cascade
    // dialog's "Continue" Button.onClicked, and unloadModuleWithDependents
    // spins a nested Qt event loop inside the QRemoteObjects teardown.
    // Running that while the click handler is still on the stack would
    // trip the QQmlData::destroyed qFatal.
    QMetaObject::invokeMethod(this, [this, moduleName]{
        // Snapshot the loaded-dependents list BEFORE the cascade runs. Once
        // unloadModuleWithDependents returns, the target is off the loaded-
        // modules list and loadedDependentsOf would come up empty. UI-plugin
        // dependents need teardown here in-process — the core cascade only
        // kills core modules (QProcess termination). Without this pass,
        // accounts_ui stays wired to a now-dead accounts_module.
        const QStringList loadedDeps = loadedDependentsOf(moduleName);

        qDebug() << "Cascade-unloading" << moduleName;
        bool ok = m_coreModuleManager
                ? m_coreModuleManager->unloadModuleWithDependents(moduleName)
                : false;
        if (!ok) {
            qWarning() << "unloadModuleWithDependents failed for" << moduleName;
            // Don't tear down the UI widget either — the core plugin is
            // still running somewhere and the widget would end up orphaned.
            return;
        }

        // Tear down any UI-plugin dependents whose backing core module just
        // died. Iterate the cached dependents (even pure-UI ones that the
        // core cascade didn't touch) and drop their widgets.
        for (const QString& dep : loadedDeps) {
            teardownUiPluginWidget(dep);
        }

        // The UI widget for the target itself still needs to be unloaded.
        // We're already inside a QueuedConnection lambda so the original
        // click handler has returned — call the impl directly instead of
        // scheduling another async hop. m_pendingUnload is inactive so the
        // cascade guard in unloadUiModuleImpl won't re-trigger.
        unloadUiModuleImpl(moduleName);

        // Stats may have shifted; the deferred-emit block that followed
        // below handles the QML-notification side.
        emit coreModulesChanged();
        emit uiModulesChanged();
        emit launcherAppsChanged();
    }, Qt::QueuedConnection);
}

void UIPluginManager::cancelUnloadCascade(const QString& moduleName)
{
    if (!m_pendingUnload.active || m_pendingUnload.name != moduleName) {
        // MainUIBackend fans out cancelPendingAction to both managers so one
        // of them is always a no-op — don't even warn here.
        return;
    }
    qDebug() << "Cancelling pending unload cascade for" << moduleName;
    m_pendingUnload = {};
}

// The in-process QObject that could carry the aboutToUnload() hook for
// `moduleName`, or nullptr when there is none.
//
// Only LEGACY (type: ui) plugins have one. m_loadedUiModules is populated
// exclusively from PluginLoader's Legacy branch — its ui_qml branch emits
// pluginLoaded() with a null IComponent* (PluginLoader.cpp:385) — so a
// non-null entry here is always an in-process QPluginLoader instance living in
// this address space.
//
// ui_qml gets nullptr, and that is a structural fact rather than an omission
// on this path. A ui_qml backend plugin is loaded by QPluginLoader inside the
// ui-host CHILD PROCESS (logos-view-module-runtime/ui-host/main.cpp). All
// Basecamp holds is the QQuickWidget showing its QML view — QML, not a QObject
// plugin — and a ViewModuleHost, which is a QProcess wrapper, not the plugin.
// There is nothing here to ask, and asking the wrapper would be theatre. That
// path's grace period belongs in ui-host, between its app.exec() returning and
// `delete pluginObject`, which is the same seam logos_host.cpp:409 uses, and
// the budget for it already exists: ViewModuleHost::stop() gives the child
// 3000ms to exit before it resorts to kill(). That is where ui-host runs
// logos::runPluginAboutToUnload — so a ui_qml backend DOES get the hook, just
// not from this process.
//
// IComponent is a plain interface (Q_DECLARE_INTERFACE, no QObject base), so
// the QObject has to be recovered by cross-cast rather than held directly.
// The concrete plugin class multiply-inherits QObject, which is what makes the
// dynamic_cast succeed.
QObject* UIPluginManager::unloadHookTarget(const QString& moduleName) const
{
    return dynamic_cast<QObject*>(m_loadedUiModules.value(moduleName));
}

void UIPluginManager::teardownUiPluginWidget(const QString& moduleName)
{
    // Idempotent — each of these maps may or may not hold an entry. Nothing
    // below cares about insertion order; we just drop every structural
    // reference the UI side may hold.
    const bool wasLoaded = m_loadedUiModules.contains(moduleName)
                        || m_qmlPluginWidgets.contains(moduleName)
                        || m_uiModuleWidgets.contains(moduleName)
                        || m_viewModuleHosts.contains(moduleName);
    if (!wasLoaded) return;

    if (m_deferredTeardowns.contains(moduleName)) {
        if (m_deferredTeardowns.value(moduleName).widget.data()
                == m_uiModuleWidgets.value(moduleName)) {
            // A deferral for this exact widget is already in flight. The maps
            // are still populated — that is exactly what wasLoaded just saw —
            // but the decision to tear down has been taken and is waiting on
            // unloadFinished() or the deadline. Returning here is what keeps
            // the documented idempotence honest in both directions: a second
            // call starts no second teardown, and it does not tear down
            // underneath the one already running either.
            qDebug() << "Teardown of" << moduleName << "already deferred; ignoring re-entry";
            return;
        }
        // Stale: it guards a widget that is no longer the loaded one, which
        // means the module was torn down by some other path and reloaded
        // inside the grace period. Drop it before arming a new deferral —
        // leaving it would let its deadline fire into the FRESH entry and tear
        // down a widget that had only just been asked.
        qDebug() << "Discarding stale deferred teardown of" << moduleName
                 << "— the module was reloaded while it waited";
        takeDeferredTeardown(moduleName);
    }

    // Ask the plugin BY NAME, never through the vtable, and that is the whole
    // reason this is shaped the way it is. IComponent — the interface every
    // legacy UI plugin is compiled against — is a header, compiled separately
    // into every plugin binary; adding a virtual to it would shift the vtable
    // under every plugin already built and turn a missing hook into undefined
    // behaviour instead of a no-op. initLogos is delivered by name for the
    // same reason (ui-host/main.cpp does exactly this).
    //
    // A plugin that does not declare the hook simply has no such meta-method
    // and we tear down as before. That is the common case — today it is EVERY
    // case — and it stays free, including free of log noise: the index probe
    // is there because invokeMethod on a missing method is a qWarning, not a
    // quiet false, and every teardown of every existing plugin would print
    // one. ui-host guards its initLogos call the same way.
    QObject* plugin = unloadHookTarget(moduleName);
    int flag = 0;  // LogosShutdown::Synchronous
    const bool answered =
        plugin
        && plugin->metaObject()->indexOfMethod("aboutToUnload()") != -1
        && QMetaObject::invokeMethod(plugin, "aboutToUnload",
                                     Qt::DirectConnection, Q_RETURN_ARG(int, flag));

    if (!answered || flag == 0) {
        teardownUiPluginWidgetNow(moduleName);
        return;
    }

    // Asynchronous, but we are on the shutdown path: ~UIPluginManager runs
    // after QCoreApplication::exec() has returned, so neither a queued
    // unloadFinished() nor a QTimer deadline can ever be delivered. Deferring
    // here would not be slow, it would be permanent. The plugin still got told
    // it is going away, which is the half of the hook that works without an
    // event loop; it just does not get the wait.
    if (m_shuttingDown) {
        qWarning() << "UI plugin" << moduleName
                   << "returned Asynchronous from aboutToUnload() during shutdown;"
                      " no event loop left to wait on, tearing down now";
        teardownUiPluginWidgetNow(moduleName);
        return;
    }

    if (!beginDeferredTeardown(moduleName, plugin)) {
        teardownUiPluginWidgetNow(moduleName);
        return;
    }

    // Deferred. teardownUiPluginWidgetNow runs from resumeDeferredTeardown.
}

// Arm the unloadFinished()/deadline race.
//
// This deliberately does NOT call logos::runPluginAboutToUnload
// (logos-plugin-qt/cpp/logos_plugin_unload.h), the shared helper the two
// out-of-process hosts use, and its own header says why: "Call this AFTER the
// application event loop has returned […] the nested event loop below is only
// safe once the outer exec() is done." Neither host it serves has an outer
// loop left — logos_host runs it after QtApp::exec() returns, ui-host after
// app.exec() returns. We are the opposite case in every respect: the live UI
// thread, inside a running application, typically from a user action, and the
// body we are guarding destroys widgets (pluginWindowRemoveRequested,
// destroyWidget, deleteLater, delete ViewModuleHost). Re-entering the event
// loop in the middle of widget destruction is the class of bug that has
// already cost this codebase a SIGSEGV — the QtRO read-stack re-entrancy in
// the wallet, fixed by deferring through QTimer::singleShot(0). So the
// algorithm is the same as the helper's and the wait is not: defer, never
// block.
bool UIPluginManager::beginDeferredTeardown(const QString& moduleName, QObject* plugin)
{
    // The widget is the staleness token the continuation re-validates against,
    // so a deferral without one has no way to tell "still mine" from "someone
    // else's". PluginLoader never emits pluginLoaded for a legacy plugin
    // without a widget (PluginLoader.cpp:193-206), so this is a guard, not a
    // path we expect to take.
    QWidget* widget = m_uiModuleWidgets.value(moduleName);
    if (!widget) return false;

    DeferredTeardown deferred;
    deferred.widget = widget;
    deferred.plugin = plugin;

    // Reached by NAME for the same ABI reason as the hook itself. A plugin
    // that says Asynchronous but exposes no way to say it is done would
    // otherwise cost every one of its teardowns the full grace period waiting
    // for a signal that cannot arrive — so we say so and fall back to
    // synchronous.
    deferred.finished = connect(plugin, SIGNAL(unloadFinished()),
                                this, SLOT(onUiPluginUnloadFinished()));
    if (!deferred.finished) {
        qWarning() << "UI plugin" << moduleName
                   << "returned Asynchronous from aboutToUnload() but has no"
                      " unloadFinished() signal; not waiting";
        return false;
    }

    deferred.deadline = new QTimer(this);
    deferred.deadline->setSingleShot(true);
    connect(deferred.deadline, &QTimer::timeout, this, [this, moduleName]() {
        resumeDeferredTeardown(moduleName, false);
    });
    deferred.elapsed.start();

    m_deferredTeardowns.insert(moduleName, deferred);
    deferred.deadline->start(kUnloadGraceMs);

    qDebug() << "UI plugin" << moduleName
             << "returned Asynchronous from aboutToUnload(); deferring teardown up to"
             << kUnloadGraceMs << "ms";
    return true;
}

void UIPluginManager::onUiPluginUnloadFinished()
{
    // Routed by sender() because the string-based connect() this arrives
    // through cannot carry a captured name. The map holds one entry per
    // in-flight deferral — a handful at the very most — so the scan is free.
    QObject* plugin = sender();
    if (!plugin) return;

    for (auto it = m_deferredTeardowns.cbegin(); it != m_deferredTeardowns.cend(); ++it) {
        if (it.value().plugin != plugin) continue;
        // Copy the key: resumeDeferredTeardown erases the entry, which would
        // leave a reference into the map dangling for the rest of the call.
        const QString moduleName = it.key();
        resumeDeferredTeardown(moduleName, true);
        return;
    }
    // No deferral for this sender: the deadline already won, or the module was
    // torn down by another path. Nothing to do — unloadFinished() is
    // documented as safe to call when nobody is listening.
}

// Remove the deferral for `moduleName` and disarm both of its wires, returning
// what it held (a default-constructed entry, whose `plugin` is null, when there
// was none). Removing the entry is the one-shot guard: whichever of the two
// wires lost the race finds nothing here.
UIPluginManager::DeferredTeardown
UIPluginManager::takeDeferredTeardown(const QString& moduleName)
{
    const auto it = m_deferredTeardowns.constFind(moduleName);
    if (it == m_deferredTeardowns.cend()) return {};
    const DeferredTeardown deferred = it.value();
    m_deferredTeardowns.remove(moduleName);

    // Disarm the loser explicitly rather than relying on it finding an empty
    // map, so a plugin that keeps emitting unloadFinished() cannot keep
    // waking us.
    QObject::disconnect(deferred.finished);
    if (deferred.deadline) {
        deferred.deadline->stop();
        // deleteLater, never delete: the timer owns the lambda whose frame we
        // may be standing in, and that lambda owns the QString our callers
        // still hold by reference.
        deferred.deadline->deleteLater();
    }
    return deferred;
}

void UIPluginManager::resumeDeferredTeardown(const QString& moduleName, bool finishedInTime)
{
    const DeferredTeardown deferred = takeDeferredTeardown(moduleName);
    if (!deferred.plugin) return;  // the other wire already won

    if (finishedInTime) {
        qDebug() << "UI plugin" << moduleName << "finished unloading in"
                 << deferred.elapsed.elapsed() << "ms";
    } else {
        // Loud, because it costs every teardown of this plugin the full grace
        // period and the plugin is the only thing that can fix it.
        qWarning() << "UI plugin" << moduleName << "did not finish unloading within"
                   << kUnloadGraceMs << "ms; proceeding";
    }

    // The continuation re-reads the maps instead of using anything captured
    // when the deferral was armed. Over a grace period this long the maps are
    // the only source of truth: unloadUiModuleImpl does not consult
    // m_deferredTeardowns, so a user-driven unload can have torn the module
    // down already, and a reload can have put a different widget behind the
    // same name — QPluginLoader hands back the same cached instance for a
    // reloaded library, so the IComponent* alone would not tell those apart.
    // The widget would; hence the QPointer token. A captured raw QWidget*
    // would be worse than useless here: the ui_qml branch of the body below
    // destroys through deleteLater(), so the address could already belong to
    // something else.
    if (m_uiModuleWidgets.value(moduleName) != deferred.widget.data()) {
        qDebug() << "Abandoning deferred teardown of" << moduleName
                 << "— it was torn down or reloaded while we waited";
        return;
    }

    teardownUiPluginWidgetNow(moduleName);
}

void UIPluginManager::teardownUiPluginWidgetNow(const QString& moduleName)
{
    // Re-checked rather than assumed: on the deferred path this runs up to a
    // grace period after teardownUiPluginWidget looked, and the shutdown path
    // (~UIPluginManager → unloadUiModuleImpl) can have got here first.
    const bool wasLoaded = m_loadedUiModules.contains(moduleName)
                        || m_qmlPluginWidgets.contains(moduleName)
                        || m_uiModuleWidgets.contains(moduleName)
                        || m_viewModuleHosts.contains(moduleName);
    if (!wasLoaded) return;

    qDebug() << "Tearing down UI plugin widget for" << moduleName;

    QWidget* widget = m_uiModuleWidgets.value(moduleName);
    IComponent* component = m_loadedUiModules.value(moduleName);

    // Order matters here: ask the workspace to drop the dock first so
    // the widget isn't reparented to a dying container; then destroy it
    // via the component's hook (which may own it) or deleteLater on the
    // bare QML host.
    if (widget) emit pluginWindowRemoveRequested(widget);
    if (component && widget) component->destroyWidget(widget);
    if (m_qmlPluginWidgets.contains(moduleName) && widget) widget->deleteLater();

    if (m_viewModuleHosts.contains(moduleName)) {
        m_viewModuleHosts[moduleName]->stop();
        delete m_viewModuleHosts.take(moduleName);
    }

    m_loadedUiModules.remove(moduleName);
    m_uiModuleWidgets.remove(moduleName);
    m_qmlPluginWidgets.remove(moduleName);
    m_loadedApps.remove(moduleName);

    if (m_currentVisibleApp == moduleName) {
        m_currentVisibleApp.clear();
        emit currentVisibleAppChanged();
    }
}

QString UIPluginManager::getPluginType(const QString& name) const
{
    const auto it = m_uiPluginMetadata.constFind(name);
    if (it != m_uiPluginMetadata.cend()) {
        return it->value("type").toString();
    }
    return QString();
}

bool UIPluginManager::isQmlPlugin(const QString& name) const
{
    return getPluginType(name) == QStringLiteral("ui_qml");
}

QString UIPluginManager::resolveQmlViewPath(const QVariantMap& meta) const
{
    // ui_qml contract: "view" is the QML entry point, relative to installDir.
    const QString installDir = meta.value("installDir").toString();
    const QString viewField = meta.value("view").toString();
    if (viewField.isEmpty()) return QString();
    return QDir(installDir).filePath(viewField);
}

void UIPluginManager::loadLegacyUiModule(const QString& moduleName)
{
    if (m_pluginLoader->isLoading(moduleName)) {
        qDebug() << "Module" << moduleName << "is already loading";
        return;
    }

    PluginLoadRequest request;
    request.name = moduleName;
    request.type = UIPluginType::Legacy;
    request.pluginPath = getPluginPath(moduleName);
    request.iconPath = pluginIconUrl(moduleName, true);
    if (m_uiPluginMetadata.contains(moduleName)) {
        request.coreDependencies = m_uiPluginMetadata[moduleName].value("dependencies").toList();
    }

    m_pluginLoader->load(request);
}

bool UIPluginManager::hasBackendPlugin(const QString& name) const
{
    // True iff the ui_qml plugin ships a backend Qt plugin lib alongside its
    // QML view. For QML-only ui_qml modules, mainFilePath is empty (no
    // backend). When a backend is present, mainFilePath points at the .so/.dylib.
    if (!isQmlPlugin(name)) return false;
    const QString mainPath = m_uiPluginMetadata.value(name).value("mainFilePath").toString();
    if (mainPath.isEmpty()) return false;
    return mainPath.endsWith(QStringLiteral(".so"), Qt::CaseInsensitive)
        || mainPath.endsWith(QStringLiteral(".dylib"), Qt::CaseInsensitive)
        || mainPath.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive);
}

QStringList UIPluginManager::findAvailableUiPlugins() const
{
    return m_uiPluginMetadata.keys();
}

QStringList UIPluginManager::loadedCoreModules() const
{
    return m_coreModuleManager ? m_coreModuleManager->loadedModules() : QStringList{};
}

QStringList UIPluginManager::loadedDependentsOf(const QString& name) const
{
    const QStringList dependents = m_packageCoordinator
        ? m_packageCoordinator->dependentsOf(name)
        : QStringList{};
    if (dependents.isEmpty()) return {};

    // A dependent is "loaded" if it's running as a core module (tracked by
    // liblogos) OR currently mounted as a UI plugin in this Basecamp instance
    // (tracked by m_loadedUiModules / m_qmlPluginWidgets). Without this
    // second source, a UI plugin like wallet_ui that depends on wallet_module
    // never registered in `logos_core_get_loaded_modules()` would silently
    // disappear from the cascade dialog — making the unload look "safe"
    // when in fact wallet_ui is still mounted and would orphan.
    const QStringList loadedCore = loadedCoreModules();
    QSet<QString> loadedSet(loadedCore.cbegin(), loadedCore.cend());
    for (auto it = m_loadedUiModules.cbegin(); it != m_loadedUiModules.cend(); ++it) {
        loadedSet.insert(it.key());
    }
    for (auto it = m_qmlPluginWidgets.cbegin(); it != m_qmlPluginWidgets.cend(); ++it) {
        loadedSet.insert(it.key());
    }

    QStringList result;
    for (const QString& dep : dependents) {
        if (loadedSet.contains(dep)) result << dep;
    }
    return result;
}

QString UIPluginManager::getPluginPath(const QString& name) const
{
    // Only used by loadLegacyUiModule (type "ui"); ui_qml uses installDir + view.
    const auto it = m_uiPluginMetadata.constFind(name);
    if (it != m_uiPluginMetadata.cend()) {
        return it->value("mainFilePath").toString();
    }
    return QString();
}

QString UIPluginManager::pluginIconUrl(const QString& pluginName, bool forWidgetIcon) const
{
    if (!m_uiPluginMetadata.contains(pluginName)) {
        return "";
    }

    const QVariantMap& meta = m_uiPluginMetadata[pluginName];
    QString iconPath = meta.value("icon").toString();
    QString installDir = meta.value("installDir").toString();

    if (iconPath.isEmpty()) {
        return "";
    }

    QDir pluginDir(installDir);
    QString filePath = pluginDir.filePath(iconPath.startsWith(":/") ? iconPath.mid(2) : iconPath);
    bool exists = QFile::exists(filePath);

    if (forWidgetIcon) {
        if (exists) {
            return filePath;
        }
        if (iconPath.startsWith(":/")) {
            qWarning() << "Plugin icon not on disk, using resource path; expected:" << filePath;
            return iconPath;
        }
        qWarning() << "Plugin icon not found, expected:" << filePath;
        return QString();
    }
    if (!exists) {
        return iconPath.startsWith(":/") ? "qrc" + iconPath : QString();
    }
    const qint64 mtimeMs = QFileInfo(filePath).lastModified().toMSecsSinceEpoch();
    return QUrl::fromLocalFile(filePath).toString()
         + QStringLiteral("?v=") + QString::number(mtimeMs);
}

QString UIPluginManager::pluginManifestVersion(const QString& pluginName) const
{
    return m_uiPluginMetadata.value(pluginName).value("manifestVersion").toString();
}

QStringList UIPluginManager::intersectWithLoaded(const QStringList& moduleNames) const
{
    if (moduleNames.isEmpty()) return {};

    // Build the same "loaded set" used by loadedDependentsOf: core modules
    // reported by liblogos plus UI-plugin widgets mounted in this process.
    // A UI-only plugin (ui_qml) won't show up in loadedCoreModules, so we
    // must merge both sources or we'd under-report what's actually loaded.
    const QStringList loadedCore = loadedCoreModules();
    QSet<QString> loadedSet(loadedCore.cbegin(), loadedCore.cend());
    for (auto it = m_loadedUiModules.cbegin(); it != m_loadedUiModules.cend(); ++it) {
        loadedSet.insert(it.key());
    }
    for (auto it = m_qmlPluginWidgets.cbegin(); it != m_qmlPluginWidgets.cend(); ++it) {
        loadedSet.insert(it.key());
    }

    QStringList result;
    for (const QString& name : moduleNames) {
        if (loadedSet.contains(name)) result << name;
    }
    return result;
}
