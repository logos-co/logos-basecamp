#include "window.h"
#include "logos_api.h"
#include "token_manager.h"
#include "logos_mode.h"
#include "LogosBasecampPaths.h"
#ifdef ENABLE_QML_INSPECTOR
#include "inspectorserver.h"
#endif
#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QTimer>
#include <QStandardPaths>
#include <iostream>
#include <memory>
#include <QStringList>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include "logos_provider_object.h"
#include "qt_provider_object.h"

// Replace CoreManager with direct C API functions
extern "C" {
    void logos_core_set_plugins_dir(const char* plugins_dir);
    void logos_core_add_plugins_dir(const char* plugins_dir);
    void logos_core_start();
    void logos_core_cleanup();
    char** logos_core_get_loaded_plugins();
    int logos_core_load_plugin(const char* plugin_name);
    char* logos_core_process_plugin(const char* plugin_path);
    char* logos_core_get_module_stats();
}

// Helper function to convert C-style array to QStringList
QStringList convertPluginsToStringList(char** plugins) {
    QStringList result;
    if (plugins) {
        for (int i = 0; plugins[i] != nullptr; i++) {
            result.append(plugins[i]);
        }
    }
    return result;
}

int main(int argc, char *argv[])
{
    // Set logos mode to Local for testing
    //LogosModeConfig::setMode(LogosMode::Local);

    // Create QApplication first
    QApplication app(argc, argv);
    app.setOrganizationName("Logos");
    app.setApplicationName("LogosBasecamp");

    // Set up module directories for logos core.
    // 1. Embedded modules directory (pre-installed at build time, read-only)
    QString embeddedModulesDir = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../modules");
    logos_core_set_plugins_dir(embeddedModulesDir.toUtf8().constData());

    // 2. User-writable modules directory (for runtime installs via the package store)
    QString userModulesDir = LogosBasecampPaths::modulesDirectory();
    logos_core_add_plugins_dir(userModulesDir.toUtf8().constData());

    // Start the core
    logos_core_start();
    std::cout << "Logos Core started successfully!" << std::endl;

    bool loaded = logos_core_load_plugin("package_manager");

    if (loaded) {
        qInfo() << "package_manager plugin loaded by default.";
    } else {
        qWarning() << "Failed to load package_manager plugin by default.";
    }

    // Print loaded plugins initially
    char** loadedPlugins = logos_core_get_loaded_plugins();
    QStringList plugins = convertPluginsToStringList(loadedPlugins);

    if (plugins.isEmpty()) {
        qInfo() << "No plugins loaded.";
    } else {
        qInfo() << "Currently loaded plugins:";
        foreach (const QString &plugin, plugins) {
            qInfo() << "  -" << plugin;
        }
        qInfo() << "Total plugins:" << plugins.size();
    }

    LogosAPI logosAPI("core", nullptr);

    qDebug() << "LogosAPI: printing keys";
    QList<QString> keys = logosAPI.getTokenManager()->getTokenKeys();
    for (const QString& key : keys) {
        qDebug() << "LogosAPI: Token key:" << key << "value:" << logosAPI.getTokenManager()->getToken(key);
    }

    // Set application icon.
#ifdef Q_OS_LINUX
    // setDesktopFileName is required for Wayland compositors, which look up the
    // icon via the .desktop file name rather than honouring setWindowIcon().
    app.setDesktopFileName("logos-basecamp");
#endif
    app.setWindowIcon(QIcon(":/icons/logos.png"));

    // Don't quit when last window is closed (for system tray support)
    app.setQuitOnLastWindowClosed(false);

    // Create and show the main window
    Window mainWindow(&logosAPI);
    mainWindow.show();

#ifdef ENABLE_QML_INSPECTOR
    // Start QML Inspector server (controlled by QML_INSPECTOR_PORT env var, default 3768)
    InspectorServer::attach(&mainWindow);
#endif

    // Set up timer to poll module stats every 2 seconds
    QTimer* statsTimer = new QTimer(&app);
    QObject::connect(statsTimer, &QTimer::timeout, [&]() {
        char* stats_json = logos_core_get_module_stats();
        if (stats_json) {
            std::cout << "Module stats: " << stats_json << std::endl;
            delete[] stats_json;
        }
    });
    statsTimer->start(2000);

    // Run the application
    int result = app.exec();

    // Cleanup
    logos_core_cleanup();

    return result;
}
