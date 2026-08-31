#pragma once

#include "ICoreRuntime.h"

#include <memory>

namespace logos::qt { class QtLogosCore; }

// QtLogosCoreRuntime — the real runtime, behind Basecamp's interface.
//
// THE ONLY FILE IN BASECAMP THAT NAMES THE CORE. When the core is reworked,
// this is what changes; nothing above it moves. If a change to the core ever
// forces an edit outside this file, the seam has sprung a leak and that is
// worth fixing rather than working around.
class QtLogosCoreRuntime final : public ICoreRuntime {
public:
    QtLogosCoreRuntime(int argc, char** argv, Config config);
    ~QtLogosCoreRuntime() override;

    void        start() override;
    QStringList knownModules() const override;
    QStringList loadedModules() const override;
    bool        loadModule(const QString& name, bool withDependencies) override;
    bool        unloadModule(const QString& name, bool withDependents) override;
    void        refreshModules() override;
    QVariantList allStats() const override;

private:
    std::unique_ptr<logos::qt::QtLogosCore> m_core;
};
