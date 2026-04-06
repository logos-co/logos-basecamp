#include "MainUIBackend.h"
#include "PluginLoader.h"
#include "LogosBasecampPaths.h"
#include <QDebug>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>

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
#include <QPointer>
#include <QFileDialog>
#include "LogosQmlBridge.h"
#include "logos_sdk.h"
#include "token_manager.h"

extern "C" {
    char* logos_core_get_module_stats();
    char* logos_core_process_plugin(const char* plugin_path);
    char** logos_core_get_known_plugins();
    char** logos_core_get_loaded_plugins();
    int logos_core_load_plugin_with_dependencies(const char* plugin_name);
    int logos_core_unload_plugin(const char* plugin_name);
    void logos_core_refresh_plugins();
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

    m_pluginLoader = new PluginLoader(m_logosAPI, this);
    connect(m_pluginLoader, &PluginLoader::pluginLoaded,
            this, &MainUIBackend::onPluginLoaded);
    connect(m_pluginLoader, &PluginLoader::pluginLoadFailed,
            this, &MainUIBackend::onPluginLoadFailed);
    connect(m_pluginLoader, &PluginLoader::loadingChanged,
            this, &MainUIBackend::loadingModulesChanged);
    
    initializeSections();
    
    m_statsTimer = new QTimer(this);
    connect(m_statsTimer, &QTimer::timeout, this, &MainUIBackend::updateModuleStats);
    m_statsTimer->start(2000);
    
    refreshCoreModules();

    subscribeToPackageInstallationEvents();

    // Initial async fetch of UI plugin metadata (emits uiModulesChanged + launcherAppsChanged when done)
    fetchUiPluginMetadata();
    
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

    // Configure the package_manager module's directories so it knows where to install
    logos.package_manager.setEmbeddedModulesDirectory(LogosBasecampPaths::embeddedModulesDirectory());
    logos.package_manager.setUserModulesDirectory(LogosBasecampPaths::modulesDirectory());
    logos.package_manager.setEmbeddedUiPluginsDirectory(LogosBasecampPaths::embeddedPluginsDirectory());
    logos.package_manager.setUserUiPluginsDirectory(LogosBasecampPaths::pluginsDirectory());

    logos.package_manager.on("corePluginFileInstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "Core module file installed:" << data[0].toString();
        QTimer::singleShot(100, this, [this]() {
            refreshCoreModules();
        });
    });

    logos.package_manager.on("uiPluginFileInstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "UI plugin file installed:" << data[0].toString();
        QTimer::singleShot(100, this, [this]() {
            fetchUiPluginMetadata();
        });
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
            fetchUiPluginMetadata();
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

    if (m_pluginLoader->isLoading(moduleName)) {
        qDebug() << "Module" << moduleName << "is already loading";
        return;
    }

    PluginLoadRequest request;
    request.name = moduleName;
    request.isQml = isQmlPlugin(moduleName);
    request.pluginPath = getPluginPath(moduleName);
    request.iconPath = getPluginIconPath(moduleName, true);

    if (request.isQml) {
        request.qmlFilePath = m_uiPluginMetadata.contains(moduleName)
            ? m_uiPluginMetadata[moduleName].value("mainFilePath").toString()
            : QDir(request.pluginPath).filePath("Main.qml");
    }

    if (m_uiPluginMetadata.contains(moduleName)) {
        request.coreDependencies = m_uiPluginMetadata[moduleName].value("dependencies").toList();
    }

    m_pluginLoader->load(request);
}

void MainUIBackend::onPluginLoaded(const QString& name, QWidget* widget,
                                   IComponent* component, bool isQml)
{
    if (component)
        m_loadedUiModules[name] = component;
    if (isQml)
        m_qmlPluginWidgets[name] = qobject_cast<QQuickWidget*>(widget);
    m_uiModuleWidgets[name] = widget;
    m_loadedApps.insert(name);

    emit uiModulesChanged();
    emit launcherAppsChanged();
    emit pluginWindowRequested(widget, name);
    emit navigateToApps();

    qDebug() << "Successfully loaded UI module:" << name;
}

void MainUIBackend::onPluginLoadFailed(const QString& name, const QString& error)
{
    qWarning() << "Failed to load UI module" << name << ":" << error;
}

QStringList MainUIBackend::loadingModules() const
{
    return m_pluginLoader->loadingPlugins();
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

    // Build the set of loaded plugins for status checking
    QStringList loadedPlugins;
    char** loaded = logos_core_get_loaded_plugins();
    if (loaded) {
        for (char** p = loaded; *p != nullptr; ++p) {
            loadedPlugins << QString::fromUtf8(*p);
            delete[] *p;
        }
        delete[] loaded;
    }

    char** known = logos_core_get_known_plugins();
    if (!known) {
        return modules;
    }

    for (char** p = known; *p != nullptr; ++p) {
        QString name = QString::fromUtf8(*p);
        delete[] *p;

        QVariantMap module;
        module["name"] = name;
        module["isLoaded"] = loadedPlugins.contains(name);

        if (m_moduleStats.contains(name)) {
            module["cpu"] = m_moduleStats[name]["cpu"];
            module["memory"] = m_moduleStats[name]["memory"];
        } else {
            module["cpu"] = "0.0";
            module["memory"] = "0.0";
        }

        modules.append(module);
    }
    delete[] known;

    return modules;
}

void MainUIBackend::loadCoreModule(const QString& moduleName)
{
    qDebug() << "Loading core module:" << moduleName;

    bool success = logos_core_load_plugin_with_dependencies(moduleName.toUtf8().constData()) == 1;

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

    bool success = logos_core_unload_plugin(moduleName.toUtf8().constData()) == 1;

    if (success) {
        qDebug() << "Successfully unloaded core module:" << moduleName;
        emit coreModulesChanged();
    } else {
        qDebug() << "Failed to unload core module:" << moduleName;
    }
}

void MainUIBackend::refreshCoreModules()
{
    // Re-scan all plugin directories via logos_core C API
    logos_core_refresh_plugins();

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

    QPointer<MainUIBackend> self(this);
    logos.package_manager.installPluginAsync(filePath, false, [self](QVariant result) {
        if (!self) return;
        Q_UNUSED(result);
        self->refreshCoreModules();
        self->fetchUiPluginMetadata();
    });
}


QString MainUIBackend::getPluginType(const QString& name) const
{
    const auto it = m_uiPluginMetadata.constFind(name);
    if (it != m_uiPluginMetadata.cend()) {
        return it->value("type").toString();
    }
    return QString();
}

bool MainUIBackend::isQmlPlugin(const QString& name) const
{
    return getPluginType(name) == "ui_qml";
}

QStringList MainUIBackend::findAvailableUiPlugins() const
{
    return m_uiPluginMetadata.keys();
}

void MainUIBackend::fetchUiPluginMetadata()
{
    if (!m_logosAPI) return;

    LogosModules logos(m_logosAPI);
    QPointer<MainUIBackend> self(this);
    logos.package_manager.getInstalledUiPluginsAsync([self](QVariantList uiPlugins) {
        if (!self) return;
        self->m_uiPluginMetadata.clear();
        for (const QVariant& item : uiPlugins) {
            QVariantMap pluginInfo = item.toMap();
            QString name = pluginInfo.value("name").toString();
            QString mainFilePath = pluginInfo.value("mainFilePath").toString();
            if (!name.isEmpty() && !mainFilePath.isEmpty()) {
                self->m_uiPluginMetadata[name] = pluginInfo;
            }
        }
        emit self->uiModulesChanged();
        emit self->launcherAppsChanged();
    });
}

QString MainUIBackend::getPluginPath(const QString& name) const
{
    const auto it = m_uiPluginMetadata.constFind(name);
    if (it != m_uiPluginMetadata.cend()) {
        QString mainFilePath = it->value("mainFilePath").toString();
        // For QML plugins, the caller needs the directory, not the file
        if (isQmlPlugin(name)) {
            return QFileInfo(mainFilePath).absolutePath();
        }
        return mainFilePath;
    }

    return QString();
}

QString MainUIBackend::getPluginIconPath(const QString& pluginName, bool forWidgetIcon) const
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

