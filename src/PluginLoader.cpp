#include "PluginLoader.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMutexLocker>
#include <QPluginLoader>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickWidget>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <memory>

#include "CoreModuleManager.h"
#include "IComponent.h"
#include "LogosQmlBridge.h"
#include "logos_api.h"
#include "restricted/DenyAllNAMFactory.h"
#include "restricted/RestrictedUrlInterceptor.h"
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
        if (!depName.isEmpty()) {
            qDebug() << "Loading core dependency for" << request.name << ":" << depName;
            if (!m_coreModuleManager || !m_coreModuleManager->loadModule(depName)) {
                qWarning() << "Failed to load core dependency" << depName
                           << "for" << request.name;
                setLoading(request.name, false);
                emit pluginLoadFailed(request.name,
                    QStringLiteral("Failed to load core dependencies for ") + request.name);
                return;
            }
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

    QWidget* widget = component->createWidget(m_logosAPI);
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

    auto* bridge = new LogosQmlBridge(m_logosAPI, this);

    if (request.mainFilePath.isEmpty()) {
        loadQmlView(request, bridge, nullptr);
        return;
    }

    // Has a backend plugin — spawn a ViewModuleHost process.
    auto* viewHost = new ViewModuleHost(this);
    if (!viewHost->spawn(request.name, request.mainFilePath)) {
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
      	const QStringList qtDefaultPaths = engine->importPathList();                                                                                                                                                               
        QStringList importPaths = qtDefaultPaths;
        importPaths.prepend(request.installDir);
        const QString qmlEntryDir =
            QFileInfo(request.qmlViewPath).absolutePath();
        if (!qmlEntryDir.isEmpty() && qmlEntryDir != request.installDir)
            importPaths.prepend(qmlEntryDir);
        QString appLibDir = QDir(QCoreApplication::applicationDirPath() + "/../lib").canonicalPath();
        if (!appLibDir.isEmpty())
            importPaths.prepend(appLibDir);
        engine->setImportPathList(importPaths);

        QStringList pluginPaths = engine->pluginPathList();
        pluginPaths.prepend(request.installDir);
        engine->setPluginPathList(pluginPaths);

        engine->setNetworkAccessManagerFactory(new DenyAllNAMFactory());

        QStringList allowedRoots;
        allowedRoots << request.installDir;
        if (!qmlEntryDir.isEmpty() && qmlEntryDir != request.installDir
            && !allowedRoots.contains(qmlEntryDir))
            allowedRoots << qmlEntryDir;
        // Allow only an explicit set of shared Logos QML modules.
        if (!appLibDir.isEmpty()) {
            static const QStringList kAllowedLogosModules = {
                QStringLiteral("Theme"),
                QStringLiteral("Controls"),
                QStringLiteral("Icons"),
            };
            for (const QString& mod : kAllowedLogosModules) {
                const QString modDir = QDir(appLibDir + "/Logos/" + mod).canonicalPath();
                if (!modDir.isEmpty() && !allowedRoots.contains(modDir))
                    allowedRoots << modDir;
            }
        }
        // TODO(security): currently allows ALL of Qt's default QML module paths.
        // Before opening the platform to third-party plugin publishing, narrow
        // this to an explicit per-module allowlist
        for (const QString& p : qtDefaultPaths) {
            if (p.startsWith(QStringLiteral("qrc:"))) continue;
            const QString canon = QDir(p).canonicalPath();
            if (!canon.isEmpty() && !allowedRoots.contains(canon))
                allowedRoots << canon;
        }
        engine->addUrlInterceptor(new RestrictedUrlInterceptor(allowedRoots));
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
