#pragma once

#include <QHash>
#include <QObject>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QVariantList>

// logos::ConsumerIdentity — what logos::admitConsumer hands back. By value in
// the cache below, so it is included rather than forward-declared.
#include "logos_consumer.h"

class LogosAPI;
class IComponent;
class QWidget;
class QQuickWidget;
class ViewModuleHost;
class LogosQmlBridge;
class IntentBridgeAdapter;
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

    
    // Set by UIPluginManager once MainUIBackend has built it. Null is
    // survivable: bridges never become intent-capable and requests from them are
    // answered `unavailable`. The adapter rather than the bare
    // LogosIntentRouter, because attaching is basecamp's concern — the runtime's
    // router treats bridge POINTERS as identity and has no registration method.
    void setIntentAdapter(IntentBridgeAdapter* adapter) { m_intentAdapter = adapter; }

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
    // Every plugin basecamp loads into its own process is ADMITTED as a
    // consumer: its own isolated token store, its own minted credential, and
    // that credential registered with capability_module — in that order, by
    // logos::admitConsumer, which is the single owner of the operation.
    //
    // Handing plugins m_logosAPI — the host's "core" identity — gave each of
    // them the host's authority: the host store holds every loaded module's
    // root auth token, and the call path reads that store before it ever
    // considers minting, so a plugin's call to a module it never declared
    // authorised on the first attempt with no capability_module handshake in
    // the log at all.
    //
    // THIS USED TO BE TWO PRIVATE HELPERS, apiForPlugin() and
    // registerPluginIdentity(), spelled out here and again — differently — in
    // logos-standalone-app. The pure-QML identity bug was one of them getting
    // the ORDER wrong: the registration sat inside the has-a-backend branch,
    // below an early return, so a pure-QML plugin registered nothing. There is
    // now one implementation, in logos-plugin-qt, and no order for a host to
    // get wrong.
    //
    // Returns a falsy ConsumerIdentity when the plugin cannot be admitted.
    // That is fatal for the plugin: falling back to m_logosAPI would restore
    // exactly the escalation this exists to remove, while looking fixed.
    logos::ConsumerIdentity consumerFor(const QString& name);

    LogosAPI* m_logosAPI;

    IntentBridgeAdapter* m_intentAdapter = nullptr;
    CoreModuleManager* m_coreModuleManager;   // not owned

    // name -> that plugin's admitted identity (its LogosAPI is parented to
    // this, so owned here). Cached because a LogosAPI captures its store by
    // raw pointer and its clients cache minted tokens; rebuilding one per load
    // attempt would re-run the requestModule handshake for every target, every
    // time — and, now that a credential is registered rather than discarded,
    // would invalidate the credential the previous incarnation still holds.
    QHash<QString, logos::ConsumerIdentity> m_consumers;

    mutable QMutex m_mutex;
    QSet<QString> m_loading;
};
