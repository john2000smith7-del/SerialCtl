#pragma once
#include "json.hpp"
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace serialctl::mcp {

using Json = nlohmann::json;

// Standard and custom error codes for SerialCtl MCP
constexpr int ErrParseError = -32700;
constexpr int ErrInvalidRequest = -32600;
constexpr int ErrMethodNotFound = -32601;
constexpr int ErrInvalidParams = -32602;
constexpr int ErrInternalError = -32603;

constexpr int ErrPowerParameterForbidden = -32001;
constexpr int ErrAppNotRunning = -32002;
constexpr int ErrLoginRequired = -32003;
constexpr int ErrConfirmationRequired = -32004;
constexpr int ErrAiRecursionLimit = -32005;
constexpr int ErrHostKeyUntrusted = -32006;
constexpr int ErrPermissionDenied = -32007;

struct SecurityCheckResult {
    bool allowed = true;
    int errorCode = 0;
    std::string errorMessage;
};

class McpSecurity {
public:
    // Enforces the strict server-side power hard-whitelist and blacklist.
    // Absolutely zero parameter alteration (voltage, current, ovp, ocp, mode, channels tampering, scpi).
    static SecurityCheckResult ValidatePowerCall(const std::string& toolName, const Json& arguments,
                                                 const Json& guiSelectedChannels, bool testUnlocked) {
        // Hard blacklist for any forbidden power parameter or command
        static const char* const kForbiddenKeys[] = {
            "voltage", "current", "ovp", "ocp", "powerLimit", "power_limit",
            "mode", "series", "parallel", "tracking", "preset", "calibration",
            "reset", "scpi", "raw", "line", "voltageStep", "currentStep"
        };

        for (const auto* key : kForbiddenKeys) {
            if (arguments.is_object() && arguments.contains(key)) {
                return {false, ErrPowerParameterForbidden,
                    "POWER_PARAMETER_CHANGE_FORBIDDEN: Modifying power parameter '" + std::string(key) +
                    "' (voltage, current, OVP/OCP, mode, SCPI, or calibration) is strictly forbidden on the server."};
            }
        }

        if (toolName == "power.get") {
            return {true, 0, ""};
        }

        if (toolName == "power.action") {
            if (!arguments.is_object() || !arguments.contains("id") || !arguments["id"].is_string()) {
                return {false, ErrInvalidParams, "power.action requires a valid 'id' string parameter."};
            }
            return {true, 0, ""};
        }

        if (toolName == "power.recover") {
            if (!testUnlocked) {
                return {false, ErrPermissionDenied, "PERMISSION_DENIED: Power recovery requires local password unlock."};
            }
            return {true, 0, ""};
        }

        if (toolName == "power.output") {
            if (!testUnlocked) {
                return {false, ErrPermissionDenied, "PERMISSION_DENIED: Power output requires local password unlock."};
            }
            if (!arguments.is_object() || !arguments.contains("enabled") || !arguments["enabled"].is_boolean()) {
                return {false, ErrInvalidParams, "power.output requires boolean parameter 'enabled'."};
            }
            // If the client attempts to override or tamper with 'channels', strictly reject!
            if (arguments.contains("channels")) {
                if (arguments["channels"] != guiSelectedChannels) {
                    return {false, ErrPowerParameterForbidden,
                        "POWER_PARAMETER_CHANGE_FORBIDDEN: Modifying or overriding power channels is strictly forbidden. "
                        "power.output only operates on GUI currently selected channels."};
                }
            }
            if (guiSelectedChannels.empty()) {
                return {false, ErrPermissionDenied, "PERMISSION_DENIED: No power channels are currently checked in the GUI."};
            }
            return {true, 0, ""};
        }

        // Any other power tool is not in the whitelist
        return {false, ErrPowerParameterForbidden,
            "POWER_PARAMETER_CHANGE_FORBIDDEN: The power tool '" + toolName + "' is not exposed. "
            "Only power.get, power.output (selected channels only), power.action, and power.recover are allowed."};
    }

    // Recursively masks passwords, credentials and tokens in JSON structures
    static Json SanitizeJson(const Json& input) {
        if (input.is_object()) {
            Json sanitized = Json::object();
            for (auto it = input.begin(); it != input.end(); ++it) {
                std::string key = it.key();
                std::string lowerKey = key;
                std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                if (lowerKey.find("password") != std::string::npos ||
                    lowerKey.find("secret") != std::string::npos ||
                    lowerKey.find("privatekey") != std::string::npos ||
                    lowerKey == "token") {
                    sanitized[key] = "[PROTECTED]";
                } else {
                    sanitized[key] = SanitizeJson(it.value());
                }
            }
            return sanitized;
        } else if (input.is_array()) {
            Json sanitized = Json::array();
            for (const auto& item : input) {
                sanitized.push_back(SanitizeJson(item));
            }
            return sanitized;
        }
        return input;
    }

    // Validates that external AI calls do not exceed recursion depth
    static SecurityCheckResult ValidateAiRecursion(const Json& arguments) {
        int depth = 0;
        if (arguments.is_object() && arguments.contains("recursionDepth") && arguments["recursionDepth"].is_number()) {
            depth = arguments["recursionDepth"].get<int>();
        }
        if (depth >= 2) {
            return {false, ErrAiRecursionLimit,
                "AI_RECURSION_LIMIT_EXCEEDED: External MCP calls to internal AI cannot exceed recursion depth of 1."};
        }
        return {true, 0, ""};
    }
};

} // namespace serialctl::mcp
