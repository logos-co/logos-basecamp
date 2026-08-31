#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include "ICoreRuntime.h"
#include "logos_api.h"

class QTimer;

// CoreModuleManager — the app's module-management surface over ICoreRuntime.
//
// Every module-management call (known/loaded lists, load/unload, cascade
// unload, stats) funnels through this class; UIPluginManager,
// PackageCoordinator and MainUIBackend never reach past it.
//
// What stays here is what the facade has no basis to decide: the poll interval,
// which thread the timer lives on, when "the module set changed" is announced,
// and the QML-facing key names. The poller is a single 2s QTimer that reads
// per-module CPU/memory and emits coreModulesChanged() so the Modules tab
// re-reads via Q_PROPERTY.
class CoreModuleManager : public QObject {
    Q_OBJECT

public:
    // `core` is the process-wide facade, owned by main() and outliving this
    // object. Must not be null.
    explicit CoreModuleManager(LogosAPI* logosAPI,
                               ICoreRuntime* core,
                               QObject* parent = nullptr);
    ~CoreModuleManager() override;

    // Thin wrappers over ICoreRuntime, the seam Basecamp owns.
    QStringList knownModules() const;
    QStringList loadedModules() const;
    // Loads with forward dependencies resolved. Returns true on success.
    bool loadModule(const QString& name);
    // Returns true on success. Does NOT cascade — see
    // unloadModuleWithDependents.
    bool unloadModule(const QString& name);
    // Tears down `name` and every currently-loaded module that depends on it,
    // leaves-first. False if any step failed — the cascade may still have made
    // progress, so callers should refresh their UI state.
    bool unloadModuleWithDependents(const QString& name);

    // Cached as of the last timer tick, so up to ~2s stale; empty for modules
    // the poller hasn't seen yet.
    QVariantMap moduleStats(const QString& name) const;

    // Re-scans every plugin directory, then emits coreModulesChanged(). Called
    // by the Modules tab's Reload button and after install/uninstall reshapes
    // the known set.
    Q_INVOKABLE void refresh();

    // Introspection, serialised to JSON for QML. On failure getMethods/
    // getEvents return "[]" and callMethod returns error JSON; a disconnected
    // module is a normal transient state, not an error.
    Q_INVOKABLE QString getMethods(const QString& moduleName);
    Q_INVOKABLE QString getEvents(const QString& moduleName);
    Q_INVOKABLE QString callMethod(const QString& moduleName,
                                   const QString& methodName,
                                   const QString& argsJson);

signals:
    // Emitted by refresh() and after every stats tick. MainUIBackend forwards
    // it into its own same-named signal, which is what QML binds to.
    void coreModulesChanged();

private slots:
    void updateModuleStats();

private:
    LogosAPI*               m_logosAPI;   // not owned
    ICoreRuntime* m_core;       // not owned; owned by main()
    QTimer*                 m_statsTimer; // owned (parent=this)
    QMap<QString, QVariantMap> m_moduleStats;
};
