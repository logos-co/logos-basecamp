#pragma once

#include <QHash>
#include <QObject>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QVariantList>

class LogosAPI;
class IComponent;
class QWidget;
class QQuickWidget;
class ViewModuleHost;
class LogosQmlBridge;
class CoreModuleManager;

enum class UIPluginType {
    Legacy,
    UiQml
};

struct PluginLoadRequest {
    QString name;
    UIPluginType type = UIPluginType::Legacy;
    QString pluginPath;
    QString iconPath;
    QVariantList coreDependencies;

    // ui_qml module fields
    QString installDir;      // Module install directory (import paths root)
    QString qmlViewPath;     // Resolved QML view entry point
    QString mainFilePath;    // Backend plugin .so/.dylib path (empty if QML-only)
};

class PluginLoader : public QObject {
    Q_OBJECT

public:
    // coreModuleManager is the single owner of the logos_core_* C API.
    // Used here to load a ui plugin's core dependencies before the ui plugin
    // itself mounts. Not owned — the PluginLoader's parent (PluginManager)
    // holds a sibling pointer to the same CoreModuleManager.
    explicit PluginLoader(LogosAPI* logosAPI,
                          CoreModuleManager* coreModuleManager,
                          QObject* parent = nullptr);

    void load(const PluginLoadRequest& request);

    bool isLoading(const QString& name) const;
    QStringList loadingPlugins() const;

signals:
    void pluginLoaded(const QString& name, QWidget* widget,
                      IComponent* component, UIPluginType type,
                      ViewModuleHost* viewHost);
    void pluginLoadFailed(const QString& name, const QString& error);
    void loadingChanged();

private:
    void startLoad(const PluginLoadRequest& request);
    void loadCoreDependencies(const PluginLoadRequest& request);
    void continueLoad(const PluginLoadRequest& request);

    // legacy ui module loading
    void loadCppPluginAsync(const PluginLoadRequest& request);
    void finishCppPluginLoad(const PluginLoadRequest& request);

    // ui_qml module loading
    void loadUiQmlModule(const PluginLoadRequest& request);
    void loadQmlView(const PluginLoadRequest& request,
                     LogosQmlBridge* bridge,
                     ViewModuleHost* viewHost);
    void finishUiQmlLoad(QQuickWidget* qmlWidget,
                         const PluginLoadRequest& request,
                         LogosQmlBridge* bridge,
                         ViewModuleHost* viewHost);

    void setLoading(const QString& name, bool loading);

    // ── per-plugin identity ─────────────────────────────────────────────
    //
    // Every plugin basecamp loads into its own process gets its own LogosAPI,
    // bound to its own isolated token store. Handing plugins m_logosAPI — the
    // host's "core" identity — gave each of them the host's authority: the
    // host store holds every loaded module's root auth token, and the call
    // path reads that store before it ever considers minting, so a plugin's
    // call to a module it never declared authorised on the first attempt with
    // no capability_module handshake in the log at all.
    //
    // Returns nullptr when the identity cannot be isolated. That is fatal for
    // the plugin: falling back to m_logosAPI would restore exactly the
    // escalation this exists to remove, while looking fixed.
    LogosAPI* apiForPlugin(const QString& name);

    // Register `name` with capability_module as a known caller holding
    // `authToken`. Without this the identity's very first requestModule is
    // refused by capability_module's known-caller gate and the plugin can
    // never obtain a token for anything.
    void registerPluginIdentity(const QString& name, const QString& authToken);

    LogosAPI* m_logosAPI;
    CoreModuleManager* m_coreModuleManager;   // not owned

    // name -> that plugin's LogosAPI (parented to this, so owned here).
    // Cached because a LogosAPI captures its store by raw pointer and its
    // clients cache minted tokens; rebuilding one per load attempt would
    // re-run the requestModule handshake for every target, every time.
    QHash<QString, LogosAPI*> m_pluginApis;

    mutable QMutex m_mutex;
    QSet<QString> m_loading;
};
