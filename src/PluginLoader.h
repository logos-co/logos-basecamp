#pragma once

#include <QObject>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QUrl>
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
    void pluginPublished(const QString& name, const QUrl& viewUrl,
                            QObject* bridge, ViewModuleHost* viewHost);

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

    LogosAPI* m_logosAPI;
    CoreModuleManager* m_coreModuleManager;   // not owned

    mutable QMutex m_mutex;
    QSet<QString> m_loading;
};
