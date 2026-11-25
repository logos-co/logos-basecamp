#pragma once

// intents_module's provider contract (contracts/intent_provider.lidl), as a core
// module: take the delivery, then answer with intents_module.respond.
//
// NO trailing `// comments` on declaration lines (the parser needs a `;`).

#include <string>

#include <logos_module_context.h>

class IntentEchoProviderImpl : public LogosModuleContext {
public:
    // Answers { ok } at once; the echo goes back through respond().
    std::string deliver(const std::string& dispatch, const std::string& intent, const std::string& params_json, const std::string& requester);
};
