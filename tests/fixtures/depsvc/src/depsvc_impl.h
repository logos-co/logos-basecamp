#pragma once

#include <string>
#include "logos_module_context.h"

class DepsvcImpl : public LogosModuleContext
{
public:
    std::string getStatus();
};
