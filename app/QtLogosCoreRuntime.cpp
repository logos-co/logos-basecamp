#include "QtLogosCoreRuntime.h"

#include "QtCoreTokens.h"
#include "logos_qt_host_core.h"

QtLogosCoreRuntime::QtLogosCoreRuntime(int argc, char** argv, Config config)
{
    // Translate Basecamp's Config into the runtime's. The two are separate
    // types on purpose: ours is what the UI needs, theirs is what the runtime
    // accepts, and a rework may change the latter without touching the former.
    logos::host::LogosCore::Config coreConfig;
    coreConfig.modulesDirs         = std::move(config.modulesDirs);
    coreConfig.persistenceBasePath = std::move(config.persistenceBasePath);
    coreConfig.accessPolicyJson    = std::move(config.accessPolicyJson);
    coreConfig.bundledModulesDirs  = std::move(config.bundledModulesDirs);
    coreConfig.packageConfigJson   = std::move(config.packageConfigJson);
    coreConfig.shellName           = std::move(config.shellName);
    // Core's module tokens, into the store this app's LogosAPI reads.
    coreConfig.tokenListener       = logos::ui::saveCoreTokenToQtStore;

    m_core = std::make_unique<logos::qt::QtLogosCore>(argc, argv, std::move(coreConfig));
}

// Out-of-line so the header can forward-declare QtLogosCore rather than
// including the runtime's header — that include is what this class exists to
// contain, and letting it escape into the header would put it back in every
// translation unit that touches the interface.
QtLogosCoreRuntime::~QtLogosCoreRuntime() = default;

void         QtLogosCoreRuntime::start()                  { m_core->start(); }
QStringList  QtLogosCoreRuntime::knownModules() const     { return m_core->knownModules(); }
QStringList  QtLogosCoreRuntime::loadedModules() const    { return m_core->loadedModules(); }
void         QtLogosCoreRuntime::refreshModules()         { m_core->refreshModules(); }
QVariantList QtLogosCoreRuntime::allStats() const         { return m_core->allStats(); }
QString      QtLogosCoreRuntime::shellCredential() const  { return m_core->shellCredential(); }
QString      QtLogosCoreRuntime::admitConsumer(const QString& name)
{
    return m_core->admitConsumer(name);
}

bool QtLogosCoreRuntime::loadModule(const QString& name, LoadPolicy policy)
{
    LogosLoadDeps deps = LOGOS_LOAD_REQUIRED_AND_OPTIONAL;
    switch (policy) {
    case LoadPolicy::ModuleOnly:          deps = LOGOS_LOAD_MODULE_ONLY; break;
    case LoadPolicy::RequiredDeps:        deps = LOGOS_LOAD_REQUIRED_DEPS; break;
    case LoadPolicy::RequiredAndOptional: deps = LOGOS_LOAD_REQUIRED_AND_OPTIONAL; break;
    }
    return m_core->loadModule(name, deps);
}

bool QtLogosCoreRuntime::unloadModule(const QString& name, bool withDependents)
{
    return m_core->unloadModule(name, withDependents);
}
