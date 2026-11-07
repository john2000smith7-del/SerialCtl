#include "McpDispatcher.h"
#include <windows.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace serialctl::mcp {

namespace {
std::string CurrentTimestampIso8601() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}
}

McpDispatcher::McpDispatcher() {
    startTimeTick_ = GetTickCount64();
}

std::string McpDispatcher::Dispatch(const std::string& clientIp, const std::string& jsonRpcRequest) {
    if (jsonRpcRequest.empty()) {
        return McpProtocol::MakeErrorResponse(nullptr, ErrInvalidRequest, "Empty request").dump();
    }

    try {
        Json req = Json::parse(jsonRpcRequest);
        if (req.is_array()) {
            Json resArr = Json::array();
            for (const auto& single : req) {
                resArr.push_back(ProcessRequest(clientIp, single));
            }
            return resArr.dump();
        } else {
            return ProcessRequest(clientIp, req).dump();
        }
    } catch (const std::exception& e) {
        return McpProtocol::MakeErrorResponse(nullptr, ErrParseError, std::string("Parse error: ") + e.what()).dump();
    }
}

Json McpDispatcher::ProcessRequest(const std::string& clientIp, const Json& request) {
    if (!request.is_object()) {
        return McpProtocol::MakeErrorResponse(nullptr, ErrInvalidRequest, "Request must be a JSON object");
    }

    Json id = request.value("id", Json(nullptr));
    std::string method = request.value("method", "");

    if (method.empty()) {
        return McpProtocol::MakeErrorResponse(id, ErrMethodNotFound, "Method not specified");
    }

    if (method == "initialize") {
        return McpProtocol::HandleInitialize(id);
    }
    if (method == "notifications/initialized") {
        return Json::object();
    }
    if (method == "ping") {
        return McpProtocol::HandlePing(id);
    }
    if (method == "tools/list") {
        return McpProtocol::HandleToolsList(id);
    }
    if (method == "tools/call") {
        Json params = request.value("params", Json::object());
        std::string toolName = params.value("name", "");
        Json arguments = params.value("arguments", Json::object());

        Json toolResult = CallTool(clientIp, toolName, arguments);
        if (toolResult.contains("error") && toolResult["error"].is_object()) {
            // Return protocol-level error if security violation or not found
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", toolResult["error"]}
            };
        }
        return McpProtocol::MakeSuccessResponse(id, toolResult);
    }

    return McpProtocol::MakeErrorResponse(id, ErrMethodNotFound, "Method '" + method + "' not recognized");
}

Json McpDispatcher::CallTool(const std::string& clientIp, const std::string& toolName, const Json& arguments) {
    std::string timestamp = CurrentTimestampIso8601();
    std::string sessionId = arguments.is_object() ? arguments.value("sessionId", "") : "";

    // 1. Power Security Boundary Check
    const bool isPowerTool = (toolName.rfind("power.", 0) == 0 ||
        toolName == "boot.enter" || toolName == "power.recover" ||
        (arguments.is_object() && (arguments.contains("voltage") || arguments.contains("current") ||
                                   arguments.contains("scpi") || arguments.contains("ovp") ||
                                   arguments.contains("ocp"))));
    Json currentGuiChannels = Json::array();
    bool currentTestUnlocked = false;
    if (isPowerTool) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentTestUnlocked = testUnlocked_;
            currentGuiChannels = guiSelectedChannels_;
        }
        if (powerContextProvider_ && (!guiStatusChecker_ || guiStatusChecker_())) {
            powerContextProvider_(currentTestUnlocked, currentGuiChannels);
            std::lock_guard<std::mutex> lock(mutex_);
            testUnlocked_ = currentTestUnlocked;
            guiSelectedChannels_ = currentGuiChannels;
        }
        auto check = McpSecurity::ValidatePowerCall(toolName, arguments, currentGuiChannels, currentTestUnlocked);
        if (!check.allowed) {
            McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId,
                                          check.errorCode, check.errorMessage, arguments);
            return {
                {"error", {
                    {"code", check.errorCode},
                    {"message", check.errorMessage}
                }}
            };
        }
    }

    // 2. AI Recursion Guard Check
    if (toolName == "ai.send") {
        auto check = McpSecurity::ValidateAiRecursion(arguments);
        if (!check.allowed) {
            McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId,
                                          check.errorCode, check.errorMessage, arguments);
            return {
                {"error", {
                    {"code", check.errorCode},
                    {"message", check.errorMessage}
                }}
            };
        }
    }

    // 3. Application Lifecycle Handlers
    if (toolName == "app.status") {
        Json res = HandleAppStatus();
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, 0, "", arguments);
        return McpProtocol::MakeToolResult(res);
    }
    if (toolName == "app.health") {
        Json res = HandleAppHealth();
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, 0, "", arguments);
        return McpProtocol::MakeToolResult(res);
    }
    if (toolName == "app.launch") {
        Json res = HandleAppLaunch(arguments);
        if (res.contains("error")) {
            int code = res.value("code", ErrInternalError);
            std::string msg = res.value("message", "Launch failed");
            McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, code, msg, arguments);
            return {
                {"error", {
                    {"code", code},
                    {"message", msg}
                }}
            };
        }
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, 0, "", arguments);
        return McpProtocol::MakeToolResult(res);
    }
    if (toolName == "app.activate") {
        Json res = HandleAppActivate();
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, 0, "", arguments);
        return McpProtocol::MakeToolResult(res);
    }
    if (toolName == "app.quit") {
        Json res = HandleAppQuit(arguments);
        if (res.contains("error")) {
            int code = res.value("code", ErrConfirmationRequired);
            std::string msg = res.value("message", "Confirmation required");
            McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, code, msg, arguments);
            return {
                {"error", {
                    {"code", code},
                    {"message", msg}
                }}
            };
        }
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, 0, "", arguments);
        return McpProtocol::MakeToolResult(res);
    }
    if (toolName == "app.service_status") {
        Json res = HandleServiceStatus();
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, 0, "", arguments);
        return McpProtocol::MakeToolResult(res);
    }
    if (toolName == "app.service_configure") {
        Json res = HandleServiceConfigure(arguments);
        if (res.contains("error")) {
            int code = ErrInternalError;
            std::string msg = "Service configurator not bound";
            if (res["error"].is_object()) {
                code = res["error"].value("code", ErrInternalError);
                msg = res["error"].value("message", msg);
            }
            McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, code, msg, arguments);
            return {
                {"error", {
                    {"code", code},
                    {"message", msg}
                }}
            };
        }
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, 0, "", arguments);
        return McpProtocol::MakeToolResult(res);
    }

    // 4. Device and UI Tools (require GUI running, except offline local TFTP tools handled by McpService)
    const bool isGuiRunning = guiStatusChecker_ ? guiStatusChecker_() : false;
    const bool isOfflineCapableTftp = (toolName.rfind("tftp.", 0) == 0 && toolName != "tftp.publish_remote");
    if (!isGuiRunning && !isOfflineCapableTftp) {
        std::string err = "APP_NOT_RUNNING: SerialCtl GUI is not running. Call app.launch first to start the application.";
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, ErrAppNotRunning, err, arguments);
        return {
            {"error", {
                {"code", ErrAppNotRunning},
                {"message", err}
            }}
        };
    }

    if (!guiExecutor_) {
        std::string err = "Internal GUI executor not bound";
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, ErrInternalError, err, arguments);
        return {
            {"error", {
                {"code", ErrInternalError},
                {"message", err}
            }}
        };
    }

    Json execResult;
    try {
        Json effectiveArgs = arguments;
        if (toolName == "power.output") {
            effectiveArgs["channels"] = currentGuiChannels;
        }
        execResult = guiExecutor_(toolName, effectiveArgs);
    } catch (const std::exception& e) {
        std::string err = std::string("Execution exception: ") + e.what();
        McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, ErrInternalError, err, arguments);
        return {
            {"error", {
                {"code", ErrInternalError},
                {"message", err}
            }}
        };
    }

    // Apply sanitization if tool is ssh_history.list
    if (toolName == "ssh_history.list") {
        execResult = McpSecurity::SanitizeJson(execResult);
    }

    int resultCode = execResult.contains("error") ? -1 : 0;
    std::string errorMsg = execResult.contains("error") ? execResult["error"].dump() : "";
    McpAuditLog::Instance().Record(timestamp, clientIp, toolName, sessionId, resultCode, errorMsg, arguments);

    bool isErr = execResult.contains("error");
    return McpProtocol::MakeToolResult(execResult, isErr);
}

Json McpDispatcher::HandleAppStatus() {
    const bool isGuiRunning = guiStatusChecker_ ? guiStatusChecker_() : false;
    uint64_t uptimeMs = GetTickCount64() - startTimeTick_;

    return {
        {"serviceRunning", true},
        {"guiRunning", isGuiRunning},
        {"version", "2.0.20"},
        {"uptimeMs", uptimeMs},
        {"status", isGuiRunning ? "GUI and MCP Service Active" : "MCP Service Active (GUI Offline)"}
    };
}

Json McpDispatcher::HandleAppHealth() {
    const bool isGuiRunning = guiStatusChecker_ ? guiStatusChecker_() : false;
    return {
        {"ok", true},
        {"serviceRunning", true},
        {"guiRunning", isGuiRunning}
    };
}

Json McpDispatcher::HandleAppLaunch(const Json& arguments) {
    bool forceFocus = arguments.is_object() ? arguments.value("forceFocus", true) : true;

    if (!guiLauncher_) {
        return {
            {"error", true},
            {"code", ErrInternalError},
            {"message", "GUI launcher not configured"}
        };
    }

    std::string error;
    bool ok = guiLauncher_(forceFocus, error);
    if (!ok) {
        int code = (error.find("LOGIN_REQUIRED") != std::string::npos) ? ErrLoginRequired : ErrInternalError;
        return {
            {"error", true},
            {"code", code},
            {"message", error}
        };
    }

    return {
        {"ok", true},
        {"launched", true},
        {"guiRunning", true}
    };
}

Json McpDispatcher::HandleAppActivate() {
    if (guiLauncher_) {
        std::string err;
        guiLauncher_(true, err);
    }
    return {
        {"ok", true},
        {"activated", true}
    };
}

Json McpDispatcher::HandleAppQuit(const Json& arguments) {
    bool confirm = arguments.is_object() ? arguments.value("confirm", false) : false;
    if (!confirm) {
        return {
            {"error", true},
            {"code", ErrConfirmationRequired},
            {"message", "CONFIRMATION_REQUIRED: app.quit requires 'confirm: true' parameter."}
        };
    }

    if (guiExecutor_) {
        return guiExecutor_("app.quit", {{"confirm", true}});
    }
    return {{"ok", true}, {"quitted", true}};
}

Json McpDispatcher::HandleServiceStatus() {
    if (serviceStatusProvider_) {
        return serviceStatusProvider_();
    }
    return {
        {"autoStart", false},
        {"port", 7080},
        {"bindIp", "0.0.0.0"}
    };
}

Json McpDispatcher::HandleServiceConfigure(const Json& arguments) {
    if (serviceConfigurator_) {
        return serviceConfigurator_(arguments);
    }
    return {
        {"error", {
            {"code", ErrInternalError},
            {"message", "Service configurator not bound"}
        }}
    };
}

} // namespace serialctl::mcp
