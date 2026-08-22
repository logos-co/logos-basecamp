#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include "logos_api.h"

class QTimer;

// Forward-declared rather than included: logos_qt_host_core.h pulls in
// nlohmann/json, and this header is included by MainUIBackend, UIPluginManager,
// PackageCoordinator and PluginLoader, none of which touch the facade.
namespace logos { namespace qt { class QtLogosCore; } }

// CoreModuleManager — the app's module-management surface, over the SDK facade.
//
// Every call into liblogos (known/loaded module lists, load/unload, cascade
// unload, stats) funnels through this class. Everything else in the app
// (UIPluginManager, PackageManager, MainUIBackend) uses these thin wrappers and never touches
// the C API directly.
//
// The char*/char** marshalling and the `delete[]`-not-`free()` ownership rule
// used to live here as a hand-written `extern "C"` mirror of the C ABI. They
// now live once, in logos-qt-sdk's `logos::qt::QtLogosCore`
// (`logos_qt_host_core.h`) over logos-cpp-sdk's `logos::host::LogosCore`, and
// this class holds one rather than declaring the ABI a second time. The
// second mirror was in main.cpp; two blocks declaring the same symbols in one
// image is an ODR hazard with no diagnostic.
//
// This class deliberately keeps the parts the SDK facade has no basis to
// decide: the poll interval, which thread the timer lives on, when
// "the module set changed" is announced, and the QML-facing key names.
//
// Also owns the periodic stats poller — a single 2s QTimer that asks
// liblogos for per-module CPU/memory and emits coreModulesChanged() so the
// Modules tab re-reads via Q_PROPERTY.
class CoreModuleManager : public QObject {
    Q_OBJECT

public:
    // `core` is the process-wide core facade, owned by main() and outliving
    // this object. Must not be null.
    explicit CoreModuleManager(LogosAPI* logosAPI,
                               logos::qt::QtLogosCore* core,
                               QObject* parent = nullptr);
    ~CoreModuleManager() override;

    // Thin wrappers — each is a one-liner over the corresponding QtLogosCore
    // call. Callers get cooked Qt types; they never see a raw C string.
    QStringList knownModules() const;
    QStringList loadedModules() const;
    // Returns true on success. Loads with dependencies — the facade call
    // that also resolves forward deps before loading.
    bool loadModule(const QString& name);
    // Returns true on success. This does NOT cascade. Caller is responsible
    // for cascade semantics (see unloadModuleWithDependents).
    bool unloadModule(const QString& name);
    // Cascade variant — tears down `name` and all currently-loaded modules
    // that depend on it, leaves-first. Returns true on full success; false
    // if any individual unload step failed (the cascade may have made
    // progress — callers should still refresh their UI state).
    bool unloadModuleWithDependents(const QString& name);

    // Cached stats as of the last timer tick (may be up to ~2s stale). Empty
    // entries for modules the poller hasn't seen yet. QML renders "0.0" via
    // the caller's compose layer when absent — we return the raw map here.
    QVariantMap moduleStats(const QString& name) const;

    // Re-scan every plugin directory via the lib, then emit
    // coreModulesChanged(). Used by the Modules tab's Reload button and by
    // PackageManager after install/uninstall events reshape the known set.
    Q_INVOKABLE void refresh();

    // Introspection — serialised to JSON for QML. getMethods/getEvents return
    // "[]" and callMethod returns error JSON on failure rather than throwing.
    // Module not being connected is a normal transient state, not an error.
    Q_INVOKABLE QString getMethods(const QString& moduleName);
    Q_INVOKABLE QString getEvents(const QString& moduleName);
    Q_INVOKABLE QString callMethod(const QString& moduleName,
                                   const QString& methodName,
                                   const QString& argsJson);

signals:
    // Emitted by refresh() and after every stats-timer tick. MainUIBackend
    // forwards this into its own signal of the same name via a signal-to-
    // signal connect — QML binds to that forwarder.
    void coreModulesChanged();

private slots:
    void updateModuleStats();

private:
    LogosAPI*               m_logosAPI;   // not owned
    logos::qt::QtLogosCore* m_core;       // not owned; owned by main()
    QTimer*                 m_statsTimer; // owned (parent=this)
    QMap<QString, QVariantMap> m_moduleStats;
};
