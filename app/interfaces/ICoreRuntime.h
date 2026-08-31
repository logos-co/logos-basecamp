#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <optional>
#include <string>
#include <vector>

// ICoreRuntime — Basecamp's own view of the module runtime.
//
// The core is being reworked wholesale. This interface is the seam that keeps
// that out of the UI: QtLogosCoreRuntime is the only file that names the core,
// and FixtureCoreRuntime answers from a JSON fixture instead.
//
// Deliberately the smallest surface that satisfies Basecamp — every method here
// is something a future core must keep providing.
class ICoreRuntime {
public:
    // Everything the runtime needs before start(). Mirrors the ordering
    // constraint rather than restating it: a Config is the only way to pass
    // these, so "set after start" stops being expressible.
    struct Config {
        std::vector<std::string>   modulesDirs;
        std::string                persistenceBasePath;
        // nullopt = install no policy (enforcement off). Distinct from an empty
        // string, which some runtimes read as "clear the existing policy".
        std::optional<std::string> accessPolicyJson;
    };

    virtual ~ICoreRuntime() = default;

    // Bring the runtime up. Everything below is only meaningful afterwards.
    virtual void start() = 0;

    // Every module the runtime can see, loaded or not.
    virtual QStringList knownModules() const = 0;

    // The subset currently running.
    //
    // Load-bearing beyond the Modules tab: PackageCoordinator gates ALL of
    // package_manager's directory setup and all ten of its event subscriptions
    // on this list containing "package_manager". A runtime that under-reports
    // here leaves Basecamp silently half-configured.
    virtual QStringList loadedModules() const = 0;

    // "Ensure loaded", not "load fresh": returns true when the module ends up
    // loaded, INCLUDING when it already was. Callers use it as an idempotent
    // guard, so a strict "did I load it just now" reading breaks them.
    virtual bool loadModule(const QString& name, bool withDependencies = true) = 0;

    // withDependents=true takes down everything that transitively depends on
    // `name` first, leaves-first, so nothing is briefly left pointing at a
    // terminated parent.
    virtual bool unloadModule(const QString& name, bool withDependents = false) = 0;

    // Re-scan for modules that appeared since start(). Called after installs.
    virtual void refreshModules() = 0;

    // Per-module resource stats, one entry per RUNNING module, as maps with at
    // least "name". CoreModuleManager tolerates several spellings of the
    // cpu/memory keys across runtime versions, so an implementation need not
    // pick one.
    //
    // ONE call for the whole set, deliberately: the stats poller ticks every
    // two seconds over every known module, and a per-module accessor turns that
    // into N calls and N parses per tick.
    virtual QVariantList allStats() const = 0;
};
