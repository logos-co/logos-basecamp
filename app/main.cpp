#include "window.h"
#include "logos_api.h"
#include "token_manager.h"
#include "logos_mode.h"
#include "LogosAppPaths.h"
#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QTimer>
#include <QStandardPaths>
#include <QPluginLoader>
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

// On every launch, attempt to install any preinstall lgx package that is not yet present in the
// user's data directories at an equal-or-higher version. Uses the bundled package_manager_plugin
// library directly (via QPluginLoader), since on first boot the package manager module itself
// has not been installed yet and is not available through logos core.
static void runPreinstallIfNeeded()
{
    QString preinstallDir = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../preinstall");
    if (!QDir(preinstallDir).exists()) {
        qInfo() << "No preinstall directory found at" << preinstallDir;
        return;
    }

    QStringList lgxFiles = QDir(preinstallDir).entryList({"*.lgx"}, QDir::Files);
    if (lgxFiles.isEmpty())
        return;

    QString userModulesDir = LogosAppPaths::modulesDirectory();
    QString userPluginsDir = LogosAppPaths::pluginsDirectory();

    // Load the bundled package_manager_plugin library directly.
    QString pluginExtension;
#if defined(Q_OS_MAC)
    pluginExtension = ".dylib";
#elif defined(Q_OS_WIN)
    pluginExtension = ".dll";
#else
    pluginExtension = ".so";
#endif

    QString pmLibPath = QDir::cleanPath(
        QCoreApplication::applicationDirPath() + "/../lib/package_manager_plugin" + pluginExtension);
    QPluginLoader loader(pmLibPath);
    if (!loader.load()) {
        qWarning() << "Failed to load bundled package_manager_plugin for preinstall:"
                    << loader.errorString();
        return;
    }

    QObject* plugin = loader.instance();
    if (!plugin) {
        qWarning() << "Failed to get package_manager_plugin instance";
        return;
    }

    LogosProviderPlugin* providerPlugin = qobject_cast<LogosProviderPlugin*>(plugin);
    LogosProviderObject* provider = providerPlugin
        ? providerPlugin->createProviderObject()
        : new QtProviderObject(plugin);

    provider->callMethod("setPluginsDirectory", {userModulesDir});
    provider->callMethod("setUiPluginsDirectory", {userPluginsDir});

    for (const QString& lgxFile : lgxFiles) {
        QString lgxPath = preinstallDir + "/" + lgxFile;
        qInfo() << "Preinstalling (if needed):" << lgxPath;
        provider->callMethod("installPlugin", {lgxPath, true});
    }
    delete provider;
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
    app.setApplicationName("LogosApp");

    // Install preinstall lgx packages before starting logos core, using the bundled
    // package_manager_plugin directly. This ensures all modules are in place when
    // logos core scans the user directories on startup.
    runPreinstallIfNeeded();

    QString userModulesDir = LogosAppPaths::modulesDirectory();

    // All modules are installed to the user data directory via preinstall/ lgx packages.
    logos_core_set_plugins_dir(userModulesDir.toUtf8().constData());

    // Start the core
    logos_core_start();
    std::cout << "Logos Core started successfully!" << std::endl;

    // The core scanner doesn't recurse into subdirectories, so we manually
    // discover and process each module installed by the LGX preinstall step.
    QString platformKey;
#if defined(Q_OS_MAC)
  #if defined(Q_PROCESSOR_ARM)
    platformKey = "darwin-arm64";
  #else
    platformKey = "darwin-x86_64";
  #endif
#elif defined(Q_OS_WIN)
    platformKey = "windows-x86_64";
#elif defined(Q_OS_LINUX)
  #if defined(Q_PROCESSOR_ARM)
    platformKey = "linux-arm64";
  #else
    platformKey = "linux-x86_64";
  #endif
#endif

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
    app.setDesktopFileName("logos-app");
#endif
    app.setWindowIcon(QIcon(":/icons/logos.png"));

    // Don't quit when last window is closed (for system tray support)
    app.setQuitOnLastWindowClosed(false);

    // Create and show the main window
    Window mainWindow(&logosAPI);
    mainWindow.show();

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
