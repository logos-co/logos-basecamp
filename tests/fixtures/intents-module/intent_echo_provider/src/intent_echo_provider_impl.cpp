#include "intent_echo_provider_impl.h"

#include <logos_caller.h>
#include <logos_sdk.h>

#include <nlohmann/json.hpp>

std::string IntentEchoProviderImpl::deliver(const std::string& dispatch, const std::string& intent,
                                            const std::string& params_json, const std::string& requester)
{
    if (!logos::currentCaller().isModule("intents_module") || intent != "test.module_echo")
        return R"({"ok":false})";

    const nlohmann::json params = nlohmann::json::parse(params_json, nullptr, /*allow_exceptions=*/false);
    const std::string text = params.is_object() && params.contains("text") && params["text"].is_string()
        ? params["text"].get<std::string>() : std::string();
    const nlohmann::json data{{"echo", text}, {"requester", requester}};

    modules().bind_intents("intents_module").respondAsyncResult(dispatch, true, data.dump(), "",
        [](logos::AsyncResult<std::string>) {}, 5000);
    return R"({"ok":true})";
}
