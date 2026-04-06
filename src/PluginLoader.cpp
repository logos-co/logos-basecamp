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

#include <atomic>
#include <memory>

#include "IComponent.h"
#include "LogosQmlBridge.h"
#include "logos_api.h"
#include "restricted/DenyAllNAMFactory.h"
#include "restricted/RestrictedUrlInterceptor.h"

extern "C" {
    int logos_core_load_plugin_with_dependencies(const char* plugin_name);
}

PluginLoader::PluginLoader(LogosAPI* logosAPI, QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
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

    loadCoreDependenciesAsync(request);
}

void PluginLoader::loadCoreDependenciesAsync(const PluginLoadRequest& request)
{
    auto success = std::make_shared<std::atomic<bool>>(true);

    QThread* thread = QThread::create([success, request]() {
        for (const QVariant& dep : request.coreDependencies) {
            QString depName = dep.toString();
            if (!depName.isEmpty()) {
                qDebug() << "Loading core dependency for" << request.name << ":" << depName;
                if (logos_core_load_plugin_with_dependencies(depName.toUtf8().constData()) != 1) {
                    qWarning() << "Failed to load core dependency" << depName
                               << "for" << request.name;
                    success->store(false);
                    return;
                }
            }
        }
    });

    connect(thread, &QThread::finished, this, [this, thread, request, success]() {
        thread->deleteLater();
        if (!success->load()) {
            setLoading(request.name, false);
            emit pluginLoadFailed(request.name,
                QStringLiteral("Failed to load core dependencies for ") + request.name);
            return;
        }
        continueLoad(request);
    });

    thread->start();
}

void PluginLoader::continueLoad(const PluginLoadRequest& request)
{
    if (request.isQml) {
        loadQmlPluginAsync(request);
    } else {
        loadCppPluginAsync(request);
    }
}

// ---------- C++ plugin path ----------

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
    emit pluginLoaded(request.name, widget, component, false);
}

// ---------- QML plugin path ----------

void PluginLoader::loadQmlPluginAsync(const PluginLoadRequest& request)
{
    if (!QFile::exists(request.qmlFilePath)) {
        qWarning() << "QML file not found for plugin" << request.name << ":" << request.qmlFilePath;
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("QML file not found: ") + request.qmlFilePath);
        return;
    }

    auto* qmlWidget = new QQuickWidget;
    qmlWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);

    QQmlEngine* engine = qmlWidget->engine();
    if (engine) {
        QStringList importPaths;
        importPaths << QStringLiteral("qrc:/qt-project.org/imports");
        importPaths << QStringLiteral("qrc:/qt/qml");
        importPaths << request.pluginPath;
        QString appLibPath = QDir(
            QCoreApplication::applicationDirPath() + QStringLiteral("/../lib")).absolutePath();
        if (QDir(appLibPath).exists())
            importPaths << appLibPath;
        engine->setImportPathList(importPaths);

        engine->setPluginPathList({});

        engine->setNetworkAccessManagerFactory(new DenyAllNAMFactory());
        QStringList allowedRoots;
        allowedRoots << request.pluginPath;
        engine->addUrlInterceptor(new RestrictedUrlInterceptor(allowedRoots));
        engine->setBaseUrl(QUrl::fromLocalFile(request.pluginPath + QStringLiteral("/")));
    }

    // Use QQmlComponent::Asynchronous to compile QML in a background thread.
    // The engine caches compiled types, so the later setSource() will be fast.
    QUrl sourceUrl = QUrl::fromLocalFile(request.qmlFilePath);
    auto* preloader = new QQmlComponent(engine, sourceUrl, QQmlComponent::Asynchronous);

    auto finishOrCleanup = [this, preloader, qmlWidget, request](QQmlComponent::Status status) {
        preloader->deleteLater();
        if (status == QQmlComponent::Ready) {
            finishQmlPluginLoad(qmlWidget, request);
        } else {
            QString errors;
            for (const auto& e : preloader->errors())
                errors += e.toString() + QStringLiteral("\n");
            qWarning() << "Failed to compile QML plugin" << request.name << ":" << errors;
            qmlWidget->deleteLater();
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

void PluginLoader::finishQmlPluginLoad(QQuickWidget* qmlWidget, const PluginLoadRequest& request)
{
    auto* bridge = new LogosQmlBridge(m_logosAPI, qmlWidget);
    qmlWidget->rootContext()->setContextProperty("logos", bridge);
    qmlWidget->setSource(QUrl::fromLocalFile(request.qmlFilePath));

    if (!request.iconPath.isEmpty())
        qmlWidget->setWindowIcon(QIcon(request.iconPath));

    if (qmlWidget->status() == QQuickWidget::Error) {
        QString errors;
        for (const QQmlError& e : qmlWidget->errors())
            errors += e.toString() + QStringLiteral("\n");
        qWarning() << "Failed to load QML plugin" << request.name << ":" << errors;
        qmlWidget->deleteLater();
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name, errors);
        return;
    }

    setLoading(request.name, false);
    emit pluginLoaded(request.name, qmlWidget, nullptr, true);
}
