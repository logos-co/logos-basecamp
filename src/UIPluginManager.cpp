#include "UIPluginManager.h"
#include "CoreModuleManager.h"
#include "PackageCoordinator.h"
#include "PluginLoader.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QQuickWidget>
#include <QSet>
#include <QUrl>

#include <ViewModuleHost.h>

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
    // Immediate emit — the package-state caches (installType, missingDeps)
    // on PackageCoordinator may still be refreshing, but the list of UI plugins
    // has changed now and QML should reflect that.
    emit uiModulesChanged();
    emit launcherAppsChanged();
}

QVariantList UIPluginManager::uiModules() const
{
    QVariantList modules;
    QStringList availablePlugins = findAvailableUiPlugins();

    for (const QString& pluginName : availablePlugins) {
        QVariantMap module;
        module["name"] = pluginName;
        module["isLoaded"] = m_loadedUiModules.contains(pluginName) || m_qmlPluginWidgets.contains(pluginName);
        module["isMainUi"] = (pluginName == "main_ui");
        module["iconPath"] = getPluginIconPath(pluginName);

        // Dependency-aware fields come from PackageCoordinator. If it hasn't
        // finished its async refresh yet, the accessors return empty values
        // which QML treats as "unknown — render safe defaults" (no red-cross,
        // no Uninstall button).
        const QStringList missing = m_packageCoordinator
            ? m_packageCoordinator->missingDepsOf(pluginName)
            : QStringList{};
        module["installType"] = m_packageCoordinator
            ? m_packageCoordinator->installType(pluginName)
            : QString(); // "" | "embedded" | "user"
        module["hasMissingDeps"] = !missing.isEmpty();
        module["missingDeps"] = missing;

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

    // Gate on missing core dependencies — no point attempting the load
    // if liblogos's dependency resolver will refuse it. We show the popup
    // instead of letting the user see a cryptic "plugin load failed" error.
    const QStringList missing = m_packageCoordinator
        ? m_packageCoordinator->missingDepsOf(moduleName)
        : QStringList{};
    if (!missing.isEmpty()) {
        qDebug() << "UI module" << moduleName << "has missing deps:" << missing;
        emit missingDepsPopupRequested(moduleName, missing);
        return;
    }

    if (isQmlPlugin(moduleName)) {
        const QVariantMap& meta = m_uiPluginMetadata.value(moduleName);

        PluginLoadRequest request;
        request.name = moduleName;
        request.type = UIPluginType::UiQml;
        request.installDir = meta.value("installDir").toString();
        request.qmlViewPath = resolveQmlViewPath(meta);
        request.iconPath = getPluginIconPath(moduleName, true);
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

    emit uiModulesChanged();
    emit launcherAppsChanged();
    emit pluginWindowRequested(widget, name);
    emit navigateToApps();

    qDebug() << "Successfully loaded UI module:" << name;
}

void UIPluginManager::onPluginLoadFailed(const QString& name, const QString& error)
{
    qWarning() << "Failed to load UI module" << name << ":" << error;
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
    // inside a Repeater delegate (e.g. UiModulesTab "Unload Plugin" button).
    // Emitting uiModulesChanged() synchronously causes Repeater.setModel to fire,
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
    if (!m_shuttingDown && !m_pendingUnload.active) {
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

    qDebug() << "Successfully unloaded UI module:" << moduleName;
}

void UIPluginManager::activateApp(const QString& appName)
{
    QWidget* widget = m_uiModuleWidgets.value(appName);
    if (widget) {
        emit pluginWindowActivateRequested(widget);
        emit navigateToApps();
    }
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

void UIPluginManager::onPluginWindowClosed(const QString& pluginName)
{
    qDebug() << "Plugin window closed:" << pluginName;

    // Called when user closes the plugin window (tab X or subwindow close). The MDI
    // subwindow and plugin widget are already destroyed
    if (m_loadedUiModules.contains(pluginName)) {
        m_loadedUiModules.remove(pluginName);
        m_uiModuleWidgets.remove(pluginName);
        m_loadedApps.remove(pluginName);

        emit uiModulesChanged();
        emit launcherAppsChanged();
    } else if (m_qmlPluginWidgets.contains(pluginName)) {
        // Stop view module host process if applicable
        if (m_viewModuleHosts.contains(pluginName)) {
            m_viewModuleHosts[pluginName]->stop();
            delete m_viewModuleHosts.take(pluginName);
        }

        m_qmlPluginWidgets.remove(pluginName);
        m_uiModuleWidgets.remove(pluginName);
        m_loadedApps.remove(pluginName);

        emit uiModulesChanged();
        emit launcherAppsChanged();
    }
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
        if (!m_pendingUnload.active) {
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

        QVariantMap app;
        app["name"] = pluginName;
        app["isLoaded"] = m_loadedApps.contains(pluginName);
        app["iconPath"] = getPluginIconPath(pluginName);
        // Sidebar red-cross marker source. The SidebarAppDelegate reads
        // this field directly; we don't ship the full missingDeps list
        // here because the sidebar only draws an indicator — the detailed
        // list lives behind the click-triggered popup which reads from
        // backend.uiModules.
        const QStringList missing = m_packageCoordinator
            ? m_packageCoordinator->missingDepsOf(pluginName)
            : QStringList{};
        app["hasMissingDeps"] = !missing.isEmpty();

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

    qDebug() << "Tearing down UI plugin widget for" << moduleName;

    QWidget* widget = m_uiModuleWidgets.value(moduleName);
    IComponent* component = m_loadedUiModules.value(moduleName);

    // Order matters here: ask MdiView to drop the tab first so the widget
    // isn't reparented to a dying container; then destroy it via the
    // component's hook (which may own it) or deleteLater on the bare QML host.
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
    request.iconPath = getPluginIconPath(moduleName, true);
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

QString UIPluginManager::getPluginIconPath(const QString& pluginName, bool forWidgetIcon) const
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
    return exists ? QUrl::fromLocalFile(filePath).toString() : (iconPath.startsWith(":/") ? "qrc" + iconPath : QString());
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
