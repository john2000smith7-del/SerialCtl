#pragma once

#include "json.hpp"
#include <cstddef>
#include <string>

namespace serialctl {

// Call with the protocol message limit and a validated requestId (0..128
// bytes). Bound the serialized UTF-8 envelope before queueing AND caching it.
inline nlohmann::json MakeApiResponse(const std::string &instance, const std::string &requestId,
                                     const nlohmann::json &result, const std::string &operation,
                                     std::size_t maximumBytes) {
    using Json = nlohmann::json;
    Json reply = {{"type", "response"}, {"version", 1}, {"instance", instance},
                  {"requestId", requestId}, {"ok", !result.contains("error")}};
    if (result.contains("error")) {
        const auto &error = result.at("error");
        reply["error"] = error.is_object() ? error : Json{{"code", "BACKEND_REJECTED"}, {"message", error}};
        if (!reply["error"].contains("message"))
            reply["error"]["message"] = reply["error"].value("code", "Backend error");
    } else
        reply["result"] = result;
    if (reply.dump().size() > maximumBytes) {
        reply.erase("result");
        reply["ok"] = false;
        reply["error"] = operation == "events"
            ? Json{{"code", "HISTORY_TOO_LARGE"}, {"message", "Use subscribe for bounded streamed history"}}
            : Json{{"code", "RESPONSE_TOO_LARGE"}, {"message", "Response exceeds the complete message limit"}};
    }
    return reply;
}

} // namespace serialctl
