#include "MainUIBackend.h"
#include "LogosAppPaths.h"
#include <QDebug>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QTimer>
#include <QQmlContext>
#include <QQuickWidget>
#include <QQmlEngine>
#include <QQmlError>
#include <QUrl>
#include <QIcon>
#include <QStandardPaths>
#include <QFileDialog>
#include "LogosQmlBridge.h"
#include "logos_sdk.h"
#include "token_manager.h"
#include "restricted/DenyAllNAMFactory.h"
#include "restricted/RestrictedUrlInterceptor.h"

extern "C" {
    char* logos_core_get_module_stats();
    char* logos_core_process_plugin(const char* plugin_path);
}

MainUIBackend::MainUIBackend(LogosAPI* logosAPI, QObject* parent)
    : QObject(parent)
    , m_currentActiveSectionIndex(0)
    , m_logosAPI(logosAPI)
    , m_ownsLogosAPI(false)
    , m_statsTimer(nullptr)
    , m_currentVisibleApp("")
{
    if (!m_logosAPI) {
        m_logosAPI = new LogosAPI("core", this);
        m_ownsLogosAPI = true;
    }
    
    initializeSections();
    
    m_statsTimer = new QTimer(this);
    connect(m_statsTimer, &QTimer::timeout, this, &MainUIBackend::updateModuleStats);
    m_statsTimer->start(2000);
    
    refreshUiModules();
    refreshCoreModules();
    refreshLauncherApps();
    
    subscribeToPackageInstallationEvents();
    
    qDebug() << "MainUIBackend created";
}

MainUIBackend::~MainUIBackend()
{
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

void MainUIBackend::subscribeToPackageInstallationEvents()
{
    if (!m_logosAPI) {
        return;
    }
    
    LogosAPIClient* client = m_logosAPI->getClient("package_manager");
    if (!client || !client->isConnected()) {
        return;
    }
    
    LogosModules logos(m_logosAPI);
    logos.package_manager.on("packageInstallationFinished", [this](const QVariantList& data) {
        if (data.size() < 3) {
            return;
        }
        bool success = data[1].toBool();
        
        if (success) {
            QTimer::singleShot(100, this, [this]() {
                refreshUiModules();
                refreshCoreModules();
                refreshLauncherApps();
            });
        }
    });
}

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

        // Check if we're navigating to Modules view
        const QVariantMap section = m_sections[index].toMap();
        const QString name = section.value("name").toString();
        if (name == "Modules") {
            refreshUiModules();
            refreshCoreModules();
        }
    }
}

QVariantList MainUIBackend::sections() const
{
    return m_sections;
}

QVariantList MainUIBackend::uiModules() const
{
    QVariantList modules;
    QStringList availablePlugins = findAvailableUiPlugins();
    
    for (const QString& pluginName : availablePlugins) {
        QVariantMap module;
        module["name"] = pluginName;
        module["isLoaded"] = m_loadedUiModules.contains(pluginName) || m_qmlPluginWidgets.contains(pluginName);
        module["isMainUi"] = (pluginName == "main_ui");
        module["iconPath"] = getPluginIconPath(pluginName);
        
        modules.append(module);
    }
    
    return modules;
}

void MainUIBackend::loadUiModule(const QString& moduleName)
{
    qDebug() << "Loading UI module:" << moduleName;
    
    if (m_loadedUiModules.contains(moduleName) || m_qmlPluginWidgets.contains(moduleName)) {
        qDebug() << "Module" << moduleName << "is already loaded";
        activateApp(moduleName);
        return;
    }
    
    // Load core module dependencies from metadata
    QJsonObject metadata = readPluginMetadata(moduleName);
    QJsonArray dependencies = metadata.value("dependencies").toArray();
    if (!dependencies.isEmpty() && m_logosAPI) {
        LogosModules logos(m_logosAPI);
        for (const QJsonValue& dep : dependencies) {
            QString depName = dep.toString();
            if (!depName.isEmpty()) {
                qDebug() << "Loading core module dependency for UI module" << moduleName << ":" << depName;
                bool success = logos.core_manager.loadPlugin(depName);
                if (!success) {
                    qWarning() << "Failed to load core module dependency" << depName << "for UI module" << moduleName;
                    return;
                }
            }
        }
    }
    
    QString pluginPath = getPluginPath(moduleName);
    qDebug() << "Loading plugin from:" << pluginPath;

    if (isQmlPlugin(moduleName)) {
        QJsonObject metadata = readQmlPluginMetadata(moduleName);
        QString mainFile = metadata.value("main").toString("Main.qml");
        QString qmlFilePath = QDir(pluginPath).filePath(mainFile);

        if (!QFile::exists(qmlFilePath)) {
            qWarning() << "Main QML file does not exist for plugin" << moduleName << ":" << qmlFilePath;
            return;
        }

        QQuickWidget* qmlWidget = new QQuickWidget;
        qmlWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        QQmlEngine* engine = qmlWidget->engine();
        if (engine) {
            QStringList importPaths;
            importPaths << QStringLiteral("qrc:/qt-project.org/imports");
            importPaths << QStringLiteral("qrc:/qt/qml");
            importPaths << pluginPath;     // Plugin-local imports only
            qDebug() << "=======================> QML import paths:" << importPaths;
            engine->setImportPathList(importPaths);

            QStringList pluginPaths;
            // note: commented out to keep this list empty to lock the plugin down
            // could cause issues with other QML components but it's unclear the moment
            //const QString qtPluginPath = QLibraryInfo::path(QLibraryInfo::PluginsPath);
            //if (!qtPluginPath.isEmpty()) {
            //    pluginPaths << qtPluginPath;  // Required for QtQuick C++ backends
            //}
            qDebug() << "=======================> QML plugin paths:" << pluginPaths;
            engine->setPluginPathList(pluginPaths);

            engine->setNetworkAccessManagerFactory(new DenyAllNAMFactory());
            QStringList allowedRoots;
            // allowedRoots << QStringLiteral("qrc:/qt/qml");
            allowedRoots << pluginPath;
            qDebug() << "=======================> QML allowed roots:" << allowedRoots;
            engine->addUrlInterceptor(new RestrictedUrlInterceptor(allowedRoots));
            qDebug() << "=======================> QML base url:" << QUrl::fromLocalFile(pluginPath + "/");
            engine->setBaseUrl(QUrl::fromLocalFile(pluginPath + "/"));
        }
        LogosQmlBridge* bridge = new LogosQmlBridge(m_logosAPI, qmlWidget);
        qmlWidget->rootContext()->setContextProperty("logos", bridge);
        qmlWidget->setSource(QUrl::fromLocalFile(qmlFilePath));
        qmlWidget->setWindowIcon(QIcon(getPluginIconPath(moduleName, true)));

        if (qmlWidget->status() == QQuickWidget::Error) {
            qWarning() << "Failed to load QML plugin" << moduleName;
            const auto errors = qmlWidget->errors();
            for (const QQmlError& error : errors) {
                qWarning() << error.toString();
            }
            qmlWidget->deleteLater();
            return;
        }

        m_qmlPluginWidgets[moduleName] = qmlWidget;
        m_uiModuleWidgets[moduleName] = qmlWidget;
        m_loadedApps.insert(moduleName);

        emit uiModulesChanged();
        emit launcherAppsChanged();

        emit pluginWindowRequested(qmlWidget, moduleName);
        emit navigateToApps();

        qDebug() << "Successfully loaded QML UI module:" << moduleName;
        return;
    }
    
    QPluginLoader loader(pluginPath);
    if (!loader.load()) {
        qDebug() << "Failed to load plugin:" << moduleName << "-" << loader.errorString();
        return;
    }
    
    QObject* plugin = loader.instance();
    if (!plugin) {
        qDebug() << "Failed to get plugin instance:" << moduleName << "-" << loader.errorString();
        return;
    }
    
    IComponent* component = qobject_cast<IComponent*>(plugin);
    if (!component) {
        qDebug() << "Failed to cast plugin to IComponent:" << moduleName;
        loader.unload();
        return;
    }
    
    QWidget* componentWidget = component->createWidget(m_logosAPI);
    if (!componentWidget) {
        qDebug() << "Component returned null widget:" << moduleName;
        loader.unload();
        return;
    }
    
    componentWidget->setWindowIcon(QIcon(getPluginIconPath(moduleName, true)));
    m_loadedUiModules[moduleName] = component;
    m_uiModuleWidgets[moduleName] = componentWidget;
    m_loadedApps.insert(moduleName);
    
    emit uiModulesChanged();
    emit launcherAppsChanged();
    
    emit pluginWindowRequested(componentWidget, moduleName);
    emit navigateToApps();
    
    qDebug() << "Successfully loaded UI module:" << moduleName;
}

void MainUIBackend::unloadUiModule(const QString& moduleName)
{
    qDebug() << "Unloading UI module:" << moduleName;
    
    bool isQml = m_qmlPluginWidgets.contains(moduleName);
    bool isCpp = m_loadedUiModules.contains(moduleName);

    if (!isQml && !isCpp) {
        qDebug() << "Module" << moduleName << "is not loaded";
        return;
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
    
    m_loadedUiModules.remove(moduleName);
    m_uiModuleWidgets.remove(moduleName);
    m_qmlPluginWidgets.remove(moduleName);
    m_loadedApps.remove(moduleName);
    
    emit uiModulesChanged();
    emit launcherAppsChanged();
    
    qDebug() << "Successfully unloaded UI module:" << moduleName;
}

void MainUIBackend::refreshUiModules()
{
    emit uiModulesChanged();
}

void MainUIBackend::activateApp(const QString& appName)
{
    QWidget* widget = m_uiModuleWidgets.value(appName);
    if (widget) {
        emit pluginWindowActivateRequested(widget);
        emit navigateToApps();
    }
}

void MainUIBackend::setCurrentVisibleApp(const QString& pluginName)
{
    if (m_currentVisibleApp != pluginName) {
        m_currentVisibleApp = pluginName;
        emit currentVisibleAppChanged();
        emit launcherAppsChanged();
    }
}

QString MainUIBackend::currentVisibleApp() const
{
    return m_currentVisibleApp;
}

void MainUIBackend::onPluginWindowClosed(const QString& pluginName)
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
        m_qmlPluginWidgets.remove(pluginName);
        m_uiModuleWidgets.remove(pluginName);
        m_loadedApps.remove(pluginName);

        emit uiModulesChanged();
        emit launcherAppsChanged();
    }
}

QVariantList MainUIBackend::coreModules() const
{
    QVariantList modules;
    
    if (!m_logosAPI) {
        return modules;
    }
    
    LogosAPIClient* coreManagerClient = m_logosAPI->getClient("core_manager");
    if (!coreManagerClient || !coreManagerClient->isConnected()) {
        qWarning() << "Core manager client is not available";
        return modules;
    }
    
    LogosModules logos(m_logosAPI);
    QJsonArray pluginsArray = logos.core_manager.getKnownPlugins();
    
    for (const QJsonValue& val : pluginsArray) {
        QJsonObject pluginObj = val.toObject();
        QString name = pluginObj["name"].toString();
        QVariantMap module;
        module["name"] = name;
        module["isLoaded"] = pluginObj["loaded"].toBool();
        
        if (m_moduleStats.contains(name)) {
            module["cpu"] = m_moduleStats[name]["cpu"];
            module["memory"] = m_moduleStats[name]["memory"];
        } else {
            module["cpu"] = "0.0";
            module["memory"] = "0.0";
        }
        
        modules.append(module);
    }
    
    return modules;
}

void MainUIBackend::loadCoreModule(const QString& moduleName)
{
    qDebug() << "Loading core module:" << moduleName;
    
    if (!m_logosAPI) {
        qWarning() << "LogosAPI not available";
        return;
    }
    
    LogosAPIClient* coreManagerClient = m_logosAPI->getClient("core_manager");
    if (!coreManagerClient || !coreManagerClient->isConnected()) {
        qWarning() << "Core manager client is not available";
        return;
    }
    
    LogosModules logos(m_logosAPI);
    bool success = logos.core_manager.loadPlugin(moduleName);
    
    if (success) {
        qDebug() << "Successfully loaded core module:" << moduleName;
        emit coreModulesChanged();
    } else {
        qDebug() << "Failed to load core module:" << moduleName;
    }
}

void MainUIBackend::unloadCoreModule(const QString& moduleName)
{
    qDebug() << "Unloading core module:" << moduleName;
    
    if (!m_logosAPI) {
        qWarning() << "LogosAPI not available";
        return;
    }
    
    LogosAPIClient* coreManagerClient = m_logosAPI->getClient("core_manager");
    if (!coreManagerClient || !coreManagerClient->isConnected()) {
        qWarning() << "Core manager client is not available";
        return;
    }
    
    LogosModules logos(m_logosAPI);
    bool success = logos.core_manager.unloadPlugin(moduleName);
    
    if (success) {
        qDebug() << "Successfully unloaded core module:" << moduleName;
        emit coreModulesChanged();
    } else {
        qDebug() << "Failed to unload core module:" << moduleName;
    }
}

void MainUIBackend::refreshCoreModules()
{
    QString libExtension;
#if defined(Q_OS_MAC)
    libExtension = ".dylib";
#elif defined(Q_OS_WIN)
    libExtension = ".dll";
#else
    libExtension = ".so";
#endif

    auto scanModulesDir = [&](const QString& dirPath) {
        QDir modulesDir(dirPath);
        if (!modulesDir.exists()) return;
        QStringList subdirs = modulesDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& subdir : subdirs) {
            QString subdirPath = dirPath + "/" + subdir;
            // Only process subdirectories that have a manifest.json
            QString manifestPath = subdirPath + "/manifest.json";
            QFile manifestFile(manifestPath);
            if (!manifestFile.open(QIODevice::ReadOnly)) continue;
            QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
            manifestFile.close();
            if (!doc.isObject()) continue;
            QString type = doc.object().value("type").toString();
            if ((type == "ui") || (type == "ui_qml")) continue; // Skip UI plugins
            // Use directory name to find the main library
            QString mainLibrary = doc.object().value("main").toString();
            if (mainLibrary.isEmpty()) {
                continue;
            }
            QString pluginPath = subdirPath + "/" + mainLibrary + libExtension;
            if (QFile::exists(pluginPath)) {
                logos_core_process_plugin(pluginPath.toUtf8().constData());
            }
        }
    };

    scanModulesDir(modulesDirectory());

    emit coreModulesChanged();
}

QString MainUIBackend::getCoreModuleMethods(const QString& moduleName)
{
    if (!m_logosAPI) {
        return "[]";
    }
    
    LogosAPIClient* client = m_logosAPI->getClient(moduleName);
    if (!client || !client->isConnected()) {
        return "[]";
    }
    
    QVariant result = client->invokeRemoteMethod(moduleName, "getMethods");
    if (result.canConvert<QJsonArray>()) {
        QJsonArray methods = result.toJsonArray();
        QJsonDocument doc(methods);
        return doc.toJson(QJsonDocument::Compact);
    }
    
    return "[]";
}

QString MainUIBackend::callCoreModuleMethod(const QString& moduleName, const QString& methodName, const QString& argsJson)
{
    if (!m_logosAPI) {
        return "{\"error\": \"LogosAPI not available\"}";
    }
    
    LogosAPIClient* client = m_logosAPI->getClient(moduleName);
    if (!client || !client->isConnected()) {
        return "{\"error\": \"Module not connected\"}";
    }
    
    QJsonDocument argsDoc = QJsonDocument::fromJson(argsJson.toUtf8());
    QJsonArray argsArray = argsDoc.array();
    
    QVariantList args;
    for (const QJsonValue& val : argsArray) {
        args.append(val.toVariant());
    }
    
    QVariant result;
    if (args.isEmpty()) {
        result = client->invokeRemoteMethod(moduleName, methodName);
    } else if (args.size() == 1) {
        result = client->invokeRemoteMethod(moduleName, methodName, args[0]);
    } else if (args.size() == 2) {
        result = client->invokeRemoteMethod(moduleName, methodName, args[0], args[1]);
    } else if (args.size() == 3) {
        result = client->invokeRemoteMethod(moduleName, methodName, args[0], args[1], args[2]);
    } else {
        return "{\"error\": \"Too many arguments\"}";
    }
    
    QJsonObject wrapper;
    wrapper["result"] = QJsonValue::fromVariant(result);
    QJsonDocument resultDoc(wrapper);
    return resultDoc.toJson(QJsonDocument::Compact);
}

QVariantList MainUIBackend::launcherApps() const
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
        
        apps.append(app);
    }
    
    return apps;
}

void MainUIBackend::onAppLauncherClicked(const QString& appName)
{
    qDebug() << "App launcher clicked:" << appName;

    setCurrentVisibleApp(appName);
    if (m_loadedApps.contains(appName)) {
        activateApp(appName);
    } else {
        loadUiModule(appName);
    }
}

void MainUIBackend::refreshLauncherApps()
{
    emit launcherAppsChanged();
}

void MainUIBackend::openInstallPluginDialog()
{
    QString filter = "LGX Package (*.lgx);;All Files (*)";

    QString filePath = QFileDialog::getOpenFileName(nullptr, tr("Select Plugin to Install"), QString(), filter);

    if (!filePath.isEmpty()) {
        installPluginFromPath(filePath);
    }
}

void MainUIBackend::installPluginFromPath(const QString& filePath)
{
    LogosModules logos(m_logosAPI);

    logos.package_manager.setPluginsDirectory(modulesDirectory());
    logos.package_manager.setUiPluginsDirectory(pluginsDirectory());


    logos.package_manager.installPlugin(filePath);

    refreshCoreModules();
    emit uiModulesChanged();
    emit launcherAppsChanged();
}

QString MainUIBackend::pluginsDirectory() const
{
    return LogosAppPaths::pluginsDirectory();
}

QString MainUIBackend::modulesDirectory() const
{
    return LogosAppPaths::modulesDirectory();
}

QJsonObject MainUIBackend::readPluginManifest(const QString& pluginName) const
{
    QString manifestPath = pluginsDirectory() + "/" + pluginName + "/manifest.json";
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
    manifestFile.close();
    if (!doc.isObject()) {
        return QJsonObject();
    }
    return doc.object();
}

QJsonObject MainUIBackend::readQmlPluginMetadata(const QString& pluginName) const
{
    QString userMetadataPath = pluginsDirectory() + "/" + pluginName + "/metadata.json";
    QFile userMetadataFile(userMetadataPath);
    if (userMetadataFile.exists() && userMetadataFile.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(userMetadataFile.readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            return doc.object();
        }
        qWarning() << "Failed to parse metadata for QML plugin" << pluginName << ":" << parseError.errorString();
    }

    qWarning() << "No metadata found for QML plugin" << pluginName;
    return QJsonObject();
}

QJsonObject MainUIBackend::readPluginMetadata(const QString& pluginName) const
{
    if (isQmlPlugin(pluginName)) {
        return readQmlPluginMetadata(pluginName);
    }

    // C++ plugins: dylib with embedded metadata
    QString pluginPath = getPluginPath(pluginName);
    QPluginLoader loader(pluginPath);
    QJsonObject metadata = loader.metaData();
    return metadata.value("MetaData").toObject();
}

QString MainUIBackend::getPluginType(const QString& name) const
{
    QString manifestPath = pluginsDirectory() + "/" + name + "/manifest.json";
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
    manifestFile.close();
    if (!doc.isObject()) {
        return QString();
    }
    return doc.object().value("type").toString();
}

bool MainUIBackend::isQmlPlugin(const QString& name) const
{
    return getPluginType(name) == "ui_qml";
}

QStringList MainUIBackend::findAvailableUiPlugins() const
{
    QStringList plugins;

    auto scanDirectory = [&](const QString& dirPath) {
        QDir pluginsDir(dirPath);
        
        if (!pluginsDir.exists()) {
            return;
        }

        auto addPlugin = [&](const QString& name) {
            if (!plugins.contains(name)) {
                plugins.append(name);
            }
        };

        QString libExtension;
#if defined(Q_OS_MAC)
        libExtension = ".dylib";
#elif defined(Q_OS_WIN)
        libExtension = ".dll";
#else
        libExtension = ".so";
#endif

        // Scan subdirectories for plugins (both QML and C++ plugins are in subdirectories)
        QStringList dirEntries = pluginsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& entry : dirEntries) {
            if (isQmlPlugin(entry)) {
                addPlugin(entry);
            } else {
                // Check if it's a C++ plugin (has <dirname>.<ext> inside the subdirectory)
                QString pluginLibPath = dirPath + "/" + entry + "/" + entry + libExtension;
                if (QFile::exists(pluginLibPath)) {
                    addPlugin(entry);
                }
            }
        }
    };
    
    scanDirectory(pluginsDirectory());

    return plugins;
}

QString MainUIBackend::getPluginPath(const QString& name) const
{
    if (isQmlPlugin(name)) {
        // QML plugins: return directory path (unchanged)
        return pluginsDirectory() + "/" + name;
    }

    // C++ plugins: return path to dylib inside subdirectory
    QString libExtension;
    #if defined(Q_OS_MAC)
        libExtension = ".dylib";
    #elif defined(Q_OS_WIN)
        libExtension = ".dll";
    #else
        libExtension = ".so";
    #endif

    return pluginsDirectory() + "/" + name + "/" + name + libExtension;
}

QString MainUIBackend::getPluginIconPath(const QString& pluginName, bool forWidgetIcon) const
{
    QJsonObject manifest = readPluginManifest(pluginName);
    QString iconPath = manifest.value("icon").toString();
    if (iconPath.isEmpty()) {
        return "";
    }

    QString pluginPath = pluginsDirectory() + "/" + pluginName;
    QDir pluginDir(pluginPath);
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

void MainUIBackend::updateModuleStats()
{
    char* stats_json = logos_core_get_module_stats();
    if (!stats_json) {
        return;
    }
    
    QString jsonStr = QString::fromUtf8(stats_json);
    QJsonDocument doc = QJsonDocument::fromJson(stats_json);
    free(stats_json);
    
    if (doc.isNull()) {
        qWarning() << "Failed to parse module stats JSON";
        return;
    }
    
    QJsonArray modulesArray;
    if (doc.isArray()) {
        modulesArray = doc.array();
    } else if (doc.isObject()) {
        QJsonObject root = doc.object();
        modulesArray = root["modules"].toArray();
    }
    
    for (const QJsonValue& val : modulesArray) {
        QJsonObject moduleObj = val.toObject();
        QString name = moduleObj["name"].toString();
        
        if (!name.isEmpty()) {
            QVariantMap stats;
            double cpu = moduleObj["cpu_percent"].toDouble();
            if (cpu == 0) cpu = moduleObj["cpu"].toDouble();
            
            double memory = moduleObj["memory_mb"].toDouble();
            if (memory == 0) memory = moduleObj["memory"].toDouble();
            if (memory == 0) memory = moduleObj["memory_MB"].toDouble();
            
            stats["cpu"] = QString::number(cpu, 'f', 1);
            stats["memory"] = QString::number(memory, 'f', 1);
            m_moduleStats[name] = stats;
        }
    }
    
    emit coreModulesChanged();
}

