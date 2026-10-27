#pragma once

#include "rep_module_backend_fixture_source.h"
#include "logos_module_context.h"

// The same .rep backend as standalone-backend, run as a core module
// ("backend": "module"). It leaves markers in its data folder so the test can
// see that it got a module's context and that closing the app unloaded it.
class ModuleBackendFixtureBackend : public ModuleBackendFixtureSimpleSource,
                                   public LogosModuleContext
{
public:
    int add(int a, int b) override;
    void crash() override;  // the test's way to make the backend die

protected:
    void onContextReady() override;
    LogosShutdown aboutToUnload() override;
};
