#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <QMap>
#include <QSet>
#include <QTimer>
#include "logos_api.h"
#include "logos_api_client.h"
#include "IComponent.h"

class QQuickWidget;
class PluginLoader;

class MainUIBackend : public QObject {
    Q_OBJECT
    
    // Navigation
    Q_PROPERTY(int currentActiveSectionIndex READ currentActiveSectionIndex WRITE setCurrentActiveSectionIndex NOTIFY currentActiveSectionIndexChanged)
    Q_PROPERTY(QVariantList sections READ sections CONSTANT)
    
    // UI Modules (Apps)
    Q_PROPERTY(QVariantList uiModules READ uiModules NOTIFY uiModulesChanged)
    
    // Core Modules
    Q_PROPERTY(QVariantList coreModules READ coreModules NOTIFY coreModulesChanged)
    
    // App Launcher
    Q_PROPERTY(QVariantList launcherApps READ launcherApps NOTIFY launcherAppsChanged)
    Q_PROPERTY(QString currentVisibleApp READ currentVisibleApp NOTIFY currentVisibleAppChanged)
    Q_PROPERTY(QStringList loadingModules READ loadingModules NOTIFY loadingModulesChanged)

public:
    explicit MainUIBackend(LogosAPI* logosAPI = nullptr, QObject* parent = nullptr);
    ~MainUIBackend();
    
    // Navigation
    int currentActiveSectionIndex() const;
    QVariantList sections() const;
    
    // UI Modules
    QVariantList uiModules() const;
    
    // Core Modules
    QVariantList coreModules() const;
    
    // App Launcher
    QVariantList launcherApps() const;
    QString currentVisibleApp() const;
    QStringList loadingModules() const;

public slots:
    // Navigation
    void setCurrentActiveSectionIndex(int index);
    // UI Module operations
    void loadUiModule(const QString& moduleName);
    void unloadUiModule(const QString& moduleName);
    void activateApp(const QString& appName);
    Q_INVOKABLE void installPluginFromPath(const QString& filePath);
    Q_INVOKABLE void openInstallPluginDialog();

    // Core Module operations
    void loadCoreModule(const QString& moduleName);
    void unloadCoreModule(const QString& moduleName);
    Q_INVOKABLE void refreshCoreModules();
    Q_INVOKABLE QString getCoreModuleMethods(const QString& moduleName);
    Q_INVOKABLE QString callCoreModuleMethod(const QString& moduleName, const QString& methodName, const QString& argsJson);

    // App Launcher operations
    void onAppLauncherClicked(const QString& appName);
    
    // Called when a plugin window is closed from MdiView
    void onPluginWindowClosed(const QString& pluginName);
    // Called when the active tab in MdiView changes (for launcher isVisible)
    void setCurrentVisibleApp(const QString& pluginName);

signals:
    void currentActiveSectionIndexChanged();
    void uiModulesChanged();
    void coreModulesChanged();
    void launcherAppsChanged();
    void currentVisibleAppChanged();
    void loadingModulesChanged();
    void navigateToApps();
    
    // Signals for C++ MdiView coordination
    void pluginWindowRequested(QWidget* widget, const QString& title);
    void pluginWindowRemoveRequested(QWidget* widget);
    void pluginWindowActivateRequested(QWidget* widget);

private slots:
    void onPluginLoaded(const QString& name, QWidget* widget,
                        IComponent* component, bool isQml);
    void onPluginLoadFailed(const QString& name, const QString& error);

private:
    void initializeSections();
    void subscribeToPackageInstallationEvents();
    void fetchUiPluginMetadata();
    QStringList findAvailableUiPlugins() const;
    QString getPluginPath(const QString& name) const;
    QString getPluginType(const QString& name) const;
    bool isQmlPlugin(const QString& name) const;
    void updateModuleStats();
    QString getPluginIconPath(const QString& pluginName, bool forWidgetIcon = false) const;
    
    // Navigation state
    int m_currentActiveSectionIndex;
    QVariantList m_sections;
    
    // UI Modules state
    QMap<QString, IComponent*> m_loadedUiModules;
    QMap<QString, QWidget*> m_uiModuleWidgets;
    QMap<QString, QQuickWidget*> m_qmlPluginWidgets;
    
    // Core Modules state
    QTimer* m_statsTimer;
    QMap<QString, QVariantMap> m_moduleStats;
    
    // App Launcher state
    QSet<QString> m_loadedApps;
    QString m_currentVisibleApp;
    
    // Plugin loading
    PluginLoader* m_pluginLoader;
    
    // LogosAPI
    LogosAPI* m_logosAPI;
    bool m_ownsLogosAPI;

    // Cache of UI plugin name → full metadata from package_manager.getInstalledUiPlugins()
    QMap<QString, QVariantMap> m_uiPluginMetadata;
};
