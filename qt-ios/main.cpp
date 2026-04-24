#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QDebug>
#include <QtPlugin>
#include "logos_core.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_sdk.h"

// Import static plugins - references Qt static plugin symbols, no headers needed
Q_IMPORT_PLUGIN(PackageManagerPlugin)
Q_IMPORT_PLUGIN(CapabilityModulePlugin)

class LogosBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool initialized READ isInitialized NOTIFY initializedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QStringList loadedPlugins READ loadedPlugins NOTIFY loadedPluginsChanged)
    Q_PROPERTY(QStringList knownPlugins READ knownPlugins NOTIFY knownPluginsChanged)
    Q_PROPERTY(QString lastAsyncResult READ lastAsyncResult NOTIFY lastAsyncResultChanged)
    Q_PROPERTY(bool hasPackageManager READ hasPackageManager CONSTANT)
    Q_PROPERTY(bool hasCapabilityModule READ hasCapabilityModule CONSTANT)
    Q_PROPERTY(QString testPluginCallResult READ testPluginCallResult NOTIFY testPluginCallResultChanged)
    Q_PROPERTY(QString buildTimestamp READ buildTimestamp CONSTANT)

public:
    explicit LogosBridge(QObject *parent = nullptr) : QObject(parent) {
        m_status = "Ready to initialize";
    }

    ~LogosBridge() {
        if (m_initialized) {
            logos_core_cleanup();
        }
    }

    bool isInitialized() const { return m_initialized; }
    QString status() const { return m_status; }
    QStringList loadedPlugins() const { return m_loadedPlugins; }
    QStringList knownPlugins() const { return m_knownPlugins; }
    QString lastAsyncResult() const { return m_lastAsyncResult; }
    
    bool hasPackageManager() const {
        return true;
    }
    
    bool hasCapabilityModule() const {
        return true;
    }
    
    QString testPluginCallResult() const { return m_testPluginCallResult; }
    
    QString buildTimestamp() const {
        return QString("%1 %2").arg(__DATE__).arg(__TIME__);
    }

    Q_INVOKABLE void initialize() {
        if (m_initialized) {
            setStatus("Already initialized");
            return;
        }

        setStatus("Initializing logos core...");
        
        char* argv[] = { const_cast<char*>("Logos"), nullptr };
        logos_core_init(1, argv);
        logos_core_set_mode(1);
        
        m_logosAPI = new LogosAPI("core", this);
        
        m_initialized = true;
        emit initializedChanged();
        
        QString moduleStatus = "Initialized. Static modules: package_manager capability_module ";
        setStatus(moduleStatus);
        refreshPluginList();
    }

    Q_INVOKABLE void start() {
        if (!m_initialized) {
            setStatus("Not initialized");
            return;
        }
        
        setStatus("Starting logos core...");
        
        logos_core_start();
        
        int registeredPlugins = 0;
        
        // Register plugins by name (plugins are statically linked and discovered via Qt's static plugin mechanism)
        int pmResult = logos_core_register_plugin_by_name("package_manager");
        if (pmResult) registeredPlugins++;

        int cmResult = logos_core_register_plugin_by_name("capability_module");
        if (cmResult) registeredPlugins++;

        setStatus(QString("Logos core started, %1 plugins registered").arg(registeredPlugins));
        
        QTimer::singleShot(500, this, &LogosBridge::refreshPluginList);
    }

    Q_INVOKABLE void refreshPluginList() {
        if (!m_initialized) return;
        
        m_loadedPlugins.clear();
        m_knownPlugins.clear();
        
        char** modules = logos_core_get_loaded_modules();
        if (modules) {
            for (int i = 0; modules[i] != nullptr; i++) {
                m_loadedPlugins.append(QString::fromUtf8(modules[i]));
                delete[] modules[i];
            }
            delete[] modules;
        }

        char** known = logos_core_get_known_modules();
        if (known) {
            for (int i = 0; known[i] != nullptr; i++) {
                m_knownPlugins.append(QString::fromUtf8(known[i]));
                delete[] known[i];
            }
            delete[] known;
        }
        
        emit loadedPluginsChanged();
        emit knownPluginsChanged();
        setStatus(QString("Loaded: %1, Known: %2")
            .arg(m_loadedPlugins.size())
            .arg(m_knownPlugins.size()));
    }
    
    Q_INVOKABLE void loadPlugin(const QString& name) {
        if (!m_initialized) {
            setStatus("Not initialized");
            return;
        }
        
        setStatus(QString("Loading module: %1...").arg(name));
        int result = logos_core_load_module(name.toUtf8().constData());
        
        if (result) {
            setStatus(QString("Successfully loaded: %1").arg(name));
        } else {
            setStatus(QString("Failed to load: %1").arg(name));
        }
        
        refreshPluginList();
    }

    Q_INVOKABLE void testAsyncOperation() {
        if (!m_initialized) {
            setStatus("Not initialized");
            return;
        }
        
        setStatus("Running async operation...");
        
        logos_core_async_operation("test_data", 
            [](int result, const char* message, void* user_data) {
                LogosBridge* self = static_cast<LogosBridge*>(user_data);
                self->setLastAsyncResult(QString("Result: %1, Message: %2")
                    .arg(result)
                    .arg(message ? message : "null"));
                self->setStatus("Async operation completed");
            }, 
            this);
    }

    Q_INVOKABLE void processEvents() {
        if (m_initialized) {
            logos_core_process_events();
        }
    }

    Q_INVOKABLE void callTestPluginCall() {
        if (!m_initialized) {
            setTestPluginCallResult("Error: Not initialized");
            return;
        }
        
        if (!m_logosAPI->getClient("package_manager")->isConnected()) {
            setTestPluginCallResult("Error: package_manager not loaded. Click Start first.");
            return;
        }
        
        setTestPluginCallResult("Calling package_manager:testPluginCall...");
        
        LogosModules logos(m_logosAPI);
        QString result = logos.package_manager.testPluginCall("hello24");
        
        if (!result.isEmpty()) {
            setTestPluginCallResult(QString("Success: %1").arg(result));
        } else {
            setTestPluginCallResult("Error: Method call failed or returned empty result");
        }
    }

signals:
    void initializedChanged();
    void statusChanged();
    void loadedPluginsChanged();
    void knownPluginsChanged();
    void lastAsyncResultChanged();
    void testPluginCallResultChanged();

private:
    void setStatus(const QString& status) {
        if (m_status != status) {
            m_status = status;
            emit statusChanged();
        }
    }

    void setLastAsyncResult(const QString& result) {
        if (m_lastAsyncResult != result) {
            m_lastAsyncResult = result;
            emit lastAsyncResultChanged();
        }
    }
    
    void setTestPluginCallResult(const QString& result) {
        if (m_testPluginCallResult != result) {
            m_testPluginCallResult = result;
            emit testPluginCallResultChanged();
        }
    }

    bool m_initialized = false;
    QString m_status;
    QStringList m_loadedPlugins;
    QStringList m_knownPlugins;
    QString m_lastAsyncResult;
    QString m_testPluginCallResult;
    LogosAPI* m_logosAPI = nullptr;
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    LogosBridge logosBridge;

    QQmlApplicationEngine engine;
    
    engine.rootContext()->setContextProperty("logosBridge", &logosBridge);
    
    const QUrl url(QStringLiteral("qrc:/Main.qml"));
    
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);
    
    engine.load(url);

    QTimer eventTimer;
    QObject::connect(&eventTimer, &QTimer::timeout, &logosBridge, &LogosBridge::processEvents);
    eventTimer.start(100);

    return app.exec();
}

#include "main.moc"
