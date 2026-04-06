#pragma once

#include <QObject>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QVariantList>

class LogosAPI;
class IComponent;
class QWidget;
class QQuickWidget;

struct PluginLoadRequest {
    QString name;
    QString pluginPath;
    QString qmlFilePath;
    bool isQml = false;
    QString iconPath;
    QVariantList coreDependencies;
};

class PluginLoader : public QObject {
    Q_OBJECT

public:
    explicit PluginLoader(LogosAPI* logosAPI, QObject* parent = nullptr);

    void load(const PluginLoadRequest& request);

    bool isLoading(const QString& name) const;
    QStringList loadingPlugins() const;

signals:
    void pluginLoaded(const QString& name, QWidget* widget,
                      IComponent* component, bool isQml);
    void pluginLoadFailed(const QString& name, const QString& error);
    void loadingChanged();

private:
    void startLoad(const PluginLoadRequest& request);
    void loadCoreDependenciesAsync(const PluginLoadRequest& request);
    void continueLoad(const PluginLoadRequest& request);
    void loadCppPluginAsync(const PluginLoadRequest& request);
    void loadQmlPluginAsync(const PluginLoadRequest& request);
    void finishCppPluginLoad(const PluginLoadRequest& request);
    void finishQmlPluginLoad(QQuickWidget* widget, const PluginLoadRequest& request);

    void setLoading(const QString& name, bool loading);

    LogosAPI* m_logosAPI;

    mutable QMutex m_mutex;
    QSet<QString> m_loading;
};
