#include "PluginLoader.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMutexLocker>
#include <QPluginLoader>
#include "win_dll_search.h"
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickWidget>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <memory>

#include "CoreModuleManager.h"
#include "IComponent.h"
#include "LogosQmlBridge.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "token_manager.h"
#include "restricted/QmlSandbox.h"
#include <ViewModuleHost.h>

PluginLoader::PluginLoader(LogosAPI* logosAPI,
                           CoreModuleManager* coreModuleManager,
                           QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
    , m_coreModuleManager(coreModuleManager)
{
}

void PluginLoader::load(const PluginLoadRequest& request)
{
    if (isLoading(request.name)) {
        qDebug() << "Plugin" << request.name << "is already loading";
        return;
    }

    setLoading(request.name, true);

    // Yield to the event loop so the UI can paint the loading state
    QTimer::singleShot(0, this, [this, request]() {
        startLoad(request);
    });
}

bool PluginLoader::isLoading(const QString& name) const
{
    QMutexLocker lock(&m_mutex);
    return m_loading.contains(name);
}

QStringList PluginLoader::loadingPlugins() const
{
    QMutexLocker lock(&m_mutex);
    return m_loading.values();
}

void PluginLoader::setLoading(const QString& name, bool loading)
{
    {
        QMutexLocker lock(&m_mutex);
        if (loading)
            m_loading.insert(name);
        else
            m_loading.remove(name);
    }
    emit loadingChanged();
}

LogosAPI* PluginLoader::apiForPlugin(const QString& name)
{
    if (name.isEmpty()) {
        qWarning() << "PluginLoader: refusing to build an identity for an unnamed plugin";
        return nullptr;
    }

    auto it = m_pluginApis.constFind(name);
    if (it != m_pluginApis.constEnd())
        return it.value();

    // LogosAPI::forIdentity isolates the store first and only then constructs,
    // which is the required order: LogosAPIClient captures its store as a raw
    // pointer at construction, so a client built before isolation would stay on
    // the ambient ring forever.
    LogosAPI* api = LogosAPI::forIdentity(name, this);
    if (!api) {
        qWarning() << "PluginLoader: could not give" << name
                   << "its own token store - refusing to load it with the host's authority";
        return nullptr;
    }
    m_pluginApis.insert(name, api);
    return api;
}

void PluginLoader::registerPluginIdentity(const QString& name, const QString& authToken)
{
    // Deliberately over the HOST's client: informModuleToken is accepted only
    // from the trusted core/capability channel, and the host is that channel.
    LogosAPIClient* cap = m_logosAPI
        ? m_logosAPI->getClient(QStringLiteral("capability_module"))
        : nullptr;
    if (!cap) {
        qWarning() << "PluginLoader: no capability_module client - identity" << name
                   << "will not be registered (its calls will be refused)";
        return;
    }

    const QString capToken = m_logosAPI->getTokenManager()
        ->getToken(QStringLiteral("capability_module"));
    if (capToken.isEmpty()) {
        qWarning() << "PluginLoader: no capability_module token on host —"
                      "plugin" << name
                   << "will not be registered (calls will be rejected)";
        return;
    }
    if (!cap->informModuleToken(capToken, name, authToken)) {
        qWarning() << "PluginLoader: capability_module.informModuleToken"
                      "failed for plugin" << name;
    }
}

void PluginLoader::startLoad(const PluginLoadRequest& request)
{
    if (request.coreDependencies.isEmpty()) {
        continueLoad(request);
        return;
    }

    loadCoreDependencies(request);
}

void PluginLoader::loadCoreDependencies(const PluginLoadRequest& request)
{
    // liblogos is not thread-safe for plugin loading; call only from the GUI thread.
    // Every core-plugin load goes through CoreModuleManager so the logos_core_*
    // C API is centralised in one place.
    for (const QVariant& dep : request.coreDependencies) {
        QString depName = dep.toString();
        if (depName.isEmpty())
            continue;
        if (!m_coreModuleManager) {
            qWarning() << "Failed to load core dependency" << depName
                       << "for" << request.name;
            setLoading(request.name, false);
            emit pluginLoadFailed(request.name,
                QStringLiteral("Failed to load core dependencies for ") + request.name);
            return;
        }
        qDebug() << "Loading core dependency for" << request.name << ":" << depName;
        if (!m_coreModuleManager->loadModule(depName)) {
            qWarning() << "Failed to load core dependency" << depName
                       << "for" << request.name;
            setLoading(request.name, false);
            emit pluginLoadFailed(request.name,
                QStringLiteral("Failed to load core dependencies for ") + request.name);
            return;
        }
    }
    continueLoad(request);
}

void PluginLoader::continueLoad(const PluginLoadRequest& request)
{
    switch (request.type) {
    case UIPluginType::UiQml:
        loadUiQmlModule(request);
        break;
    case UIPluginType::Legacy:
        loadCppPluginAsync(request);
        break;
    }
}

// ---------- Legacy ui plugin path ----------

void PluginLoader::loadCppPluginAsync(const PluginLoadRequest& request)
{
    // Pre-load the shared library in a background thread.
    // Qt's QLibraryStore caches loaded libraries globally, so the subsequent
    // QPluginLoader::load() on the main thread will be instant.
    QThread* thread = QThread::create([path = request.pluginPath]() {
        // Add the plugin's OWN directory to the DLL search before the image is
        // mapped, so a UI plugin can resolve libraries vendored beside it.
        // Windows searches the EXECUTABLE's directory, not the importing DLL's.
        // No-op off Windows. The reference is deliberately not released: these
        // plugins stay resident for the process lifetime (QPluginLoader's
        // destructor does not unload), and this is the load that actually maps
        // the image -- the main-thread load below then hits Qt's cache.
        ModuleLib::preloadPluginWithOwnDirSearch(path);
        QPluginLoader loader(path);
        loader.load();
    });

    connect(thread, &QThread::finished, this,
        [this, thread, request]() {
            thread->deleteLater();
            finishCppPluginLoad(request);
        });

    thread->start();
}

void PluginLoader::finishCppPluginLoad(const PluginLoadRequest& request)
{
    // Normally a no-op: loadCppPluginAsync already mapped the image. Kept so
    // this path is correct on its own, since a caller reaching it without the
    // async pre-load would otherwise fail to resolve vendored DLLs.
    ModuleLib::preloadPluginWithOwnDirSearch(request.pluginPath);
    QPluginLoader loader(request.pluginPath);
    if (!loader.load()) {
        qWarning() << "Failed to load plugin:" << request.name << "-" << loader.errorString();
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name, loader.errorString());
        return;
    }

    QObject* plugin = loader.instance();
    if (!plugin) {
        qWarning() << "Failed to get plugin instance:" << request.name;
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name, QStringLiteral("Failed to get plugin instance"));
        return;
    }

    IComponent* component = qobject_cast<IComponent*>(plugin);
    if (!component) {
        qWarning() << "Plugin does not implement IComponent:" << request.name;
        loader.unload();
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name, QStringLiteral("Plugin does not implement IComponent"));
        return;
    }

    // The plugin's own identity, not the host's. A legacy widget plugin calls
    // modules through whatever LogosAPI it is handed, so handing it m_logosAPI
    // handed it the host's ambient token ring.
    LogosAPI* pluginApi = apiForPlugin(request.name);
    if (!pluginApi) {
        loader.unload();
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("Could not establish an isolated identity for ") + request.name);
        return;
    }
    registerPluginIdentity(request.name, QUuid::createUuid().toString(QUuid::WithoutBraces));

    QWidget* widget = component->createWidget(pluginApi);
    if (!widget) {
        qWarning() << "Component returned null widget:" << request.name;
        loader.unload();
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name, QStringLiteral("Component returned null widget"));
        return;
    }

    if (!request.iconPath.isEmpty())
        widget->setWindowIcon(QIcon(request.iconPath));

    setLoading(request.name, false);
    emit pluginLoaded(request.name, widget, component, UIPluginType::Legacy, nullptr);
}

// ---------- ui_qml module path ----------

void PluginLoader::loadUiQmlModule(const PluginLoadRequest& request)
{
    if (request.qmlViewPath.isEmpty() || !QFile::exists(request.qmlViewPath)) {
        qWarning() << "ui_qml module QML file not found:" << request.qmlViewPath;
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("QML view file not found: ") + request.qmlViewPath);
        return;
    }

    // The bridge the QML gets speaks AS this module, from this module's own
    // token store — not as basecamp. Built via forIdentity so the store is
    // isolated BEFORE any client for the name exists.
    LogosAPI* pluginApi = apiForPlugin(request.name);
    if (!pluginApi) {
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("Could not establish an isolated identity for ") + request.name);
        return;
    }
    auto* bridge = new LogosQmlBridge(pluginApi, this);

    // Mint a per-spawn UUID.
    //
    // EVERY ui_qml module gets one, backend or not. Two separate things used to
    // be conflated here, and only the second one needs a ui-host:
    //
    //   1. Making `request.name` a KNOWN CALLER. capability_module's
    //      known-caller gate refuses requestModule from a name it has no token
    //      for, so without this registration an isolated identity can never
    //      obtain a token for anything. This is needed by the pure-QML path
    //      too — in fact especially there, since the QML is the only thing
    //      calling out.
    //   2. Giving ui-host the token its backend accepts inbound calls with.
    //
    // The old code did both inside the has-a-backend branch, below an early
    // return, so the pure-QML path registered nothing. It got away with it
    // because it was calling with the host's ambient ring, where every target's
    // token was already present and no handshake ever happened.
    const QString uiAuthToken = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Register the UI module's auth token with capability_module BEFORE
    // spawning ui-host. Plugin ctors commonly schedule their first IPC
    // calls via QTimer::singleShot(0, ...) which fire the instant
    // ui-host enters its event loop. If we deferred this to onHostReady
    // (called after ViewModuleHost::ready), the async IPC to
    // capability_module would race those first calls — capability_module
    // would reject them with "auth token not recognized" because the
    // token hadn't been registered yet, leaving the plugin's first
    // refresh silently empty. capability_module is fully loaded by this
    // point (loaded during basecamp startup), so the synchronous IPC
    // here is cheap and closes the race deterministically.
    registerPluginIdentity(request.name, uiAuthToken);

    if (request.mainFilePath.isEmpty()) {
        loadQmlView(request, bridge, nullptr);
        return;
    }

    // Has a backend plugin — spawn a ViewModuleHost process.
    auto* viewHost = new ViewModuleHost(this);
    if (!viewHost->spawn(request.name, request.mainFilePath, uiAuthToken)) {
        qWarning() << "Failed to spawn ui-host for ui_qml module" << request.name;
        delete viewHost;
        delete bridge;
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("Failed to spawn ui-host for ") + request.name);
        return;
    }

    auto onHostReady = [this, request, bridge, viewHost]() {
        bridge->setViewModuleSocket(request.name, viewHost->socketName());

        const QString base = QFileInfo(request.mainFilePath).absolutePath()
            + QStringLiteral("/") + request.name
            + QStringLiteral("_replica_factory");
        for (const QString& suffix : { QStringLiteral(".dylib"),
                                       QStringLiteral(".so"),
                                       QStringLiteral(".dll") }) {
            const QString factoryPath = base + suffix;
            if (QFile::exists(factoryPath)) {
                bridge->setViewReplicaPlugin(request.name, factoryPath);
                break;
            }
        }
        loadQmlView(request, bridge, viewHost);
    };

    auto* timeout = new QTimer(this);
    timeout->setSingleShot(true);
    auto readyConn = std::make_shared<QMetaObject::Connection>();
    auto timeoutConn = std::make_shared<QMetaObject::Connection>();
    *readyConn = connect(viewHost, &ViewModuleHost::ready, this,
        [timeout, readyConn, timeoutConn, onHostReady]() {
            QObject::disconnect(*readyConn);
            QObject::disconnect(*timeoutConn);
            timeout->stop();
            timeout->deleteLater();
            onHostReady();
        });
    *timeoutConn = connect(timeout, &QTimer::timeout, this,
        [this, request, viewHost, bridge, timeout, readyConn, timeoutConn]() {
            QObject::disconnect(*readyConn);
            QObject::disconnect(*timeoutConn);
            timeout->deleteLater();
            qWarning() << "Timeout waiting for ui-host ready signal for" << request.name;
            viewHost->stop();
            viewHost->deleteLater();
            delete bridge;
            setLoading(request.name, false);
            emit pluginLoadFailed(request.name,
                QStringLiteral("Timeout waiting for ui-host for ") + request.name);
        });
    timeout->start(30000);
}

void PluginLoader::loadQmlView(const PluginLoadRequest& request,
                               LogosQmlBridge* bridge,
                               ViewModuleHost* viewHost)
{
    auto* qmlWidget = new QQuickWidget;
    qmlWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    if (QQmlEngine* engine = qmlWidget->engine()) {
        const QString appLibDir =
            QDir(QCoreApplication::applicationDirPath() + "/../lib").canonicalPath();
        QmlSandbox::configure(engine, request.installDir, request.qmlViewPath,
                              appLibDir, request.name);
        engine->setBaseUrl(QUrl::fromLocalFile(request.installDir + "/"));
    }

    // Async pre-compile: the engine caches compiled types so setSource() is fast.
    QUrl sourceUrl = QUrl::fromLocalFile(request.qmlViewPath);
    auto* preloader = new QQmlComponent(qmlWidget->engine(), sourceUrl,
                                        QQmlComponent::Asynchronous);

    auto finishOrCleanup = [this, preloader, qmlWidget, request, bridge,
                            viewHost](QQmlComponent::Status status) {
        preloader->deleteLater();
        if (status == QQmlComponent::Ready) {
            finishUiQmlLoad(qmlWidget, request, bridge, viewHost);
        } else {
            QString errors;
            for (const auto& e : preloader->errors())
                errors += e.toString() + QStringLiteral("\n");
            qWarning() << "Failed to compile ui_qml view" << request.name << ":" << errors;
            qmlWidget->deleteLater();
            delete bridge;
            if (viewHost) { viewHost->stop(); delete viewHost; }
            setLoading(request.name, false);
            emit pluginLoadFailed(request.name, errors);
        }
    };

    if (preloader->isReady() || preloader->isError()) {
        finishOrCleanup(preloader->status());
    } else {
        connect(preloader, &QQmlComponent::statusChanged, this, finishOrCleanup);
    }
}

void PluginLoader::finishUiQmlLoad(QQuickWidget* qmlWidget,
                                   const PluginLoadRequest& request,
                                   LogosQmlBridge* bridge,
                                   ViewModuleHost* viewHost)
{
    bridge->setParent(qmlWidget);
    qmlWidget->rootContext()->setContextProperty("logos", bridge);
    qmlWidget->rootContext()->setContextProperty("isActiveTab", true);
    qmlWidget->setSource(QUrl::fromLocalFile(request.qmlViewPath));

    if (!request.iconPath.isEmpty())
        qmlWidget->setWindowIcon(QIcon(request.iconPath));

    if (qmlWidget->status() == QQuickWidget::Error) {
        qWarning() << "Failed to load ui_qml view" << request.name;
        const auto errors = qmlWidget->errors();
        for (const QQmlError& error : errors) qWarning() << error.toString();
        qmlWidget->deleteLater();
        if (viewHost) { viewHost->stop(); delete viewHost; }
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("Failed to load QML view for ") + request.name);
        return;
    }

    setLoading(request.name, false);
    emit pluginLoaded(request.name, qmlWidget, nullptr, UIPluginType::UiQml, viewHost);
}
