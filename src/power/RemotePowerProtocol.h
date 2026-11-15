#pragma once
#include "json.hpp"
#include <algorithm>
#include <string_view>

namespace serialctl {
inline constexpr auto RemotePowerStateOp = "power.remote.state";
inline constexpr auto RemotePowerSubmitOp = "power.remote.submit";
inline constexpr auto RemotePowerActionOp = "power.remote.action";
inline constexpr auto RemotePowerReleaseOp = "power.remote.release";

inline bool RemotePowerCommandAllowed(const nlohmann::json &command) {
    if (!command.is_object() || !command.contains("type") || !command["type"].is_string()) return false;
    const auto &type = command["type"].get_ref<const std::string &>();
    constexpr std::string_view operations[]{
        "output", "parameters", "batch", "mode", "protection", "task", "stop", "sequence",
        "schedule", "interval", "deviceTimer", "trigger", "step"
    };
    if (std::find(std::begin(operations), std::end(operations), type) != std::end(operations)) return true;
    const auto operation = command.find("operation");
    if (operation == command.end() || !operation->is_string()) return false;
    if (type == "diagnostic")
        return *operation == "identity" || *operation == "status" || *operation == "errors";
    if (type != "settings") return false;
    constexpr std::string_view settings[]{
        "readMode", "readTimer", "readProtection", "protection", "clearProtection", "readPanel",
        "panel", "beep", "clearText", "savePreset", "recallPreset"
    };
    const auto &name = operation->get_ref<const std::string &>();
    return std::find(std::begin(settings), std::end(settings), name) != std::end(settings);
}
} // namespace serialctl
