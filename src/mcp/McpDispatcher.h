#pragma once
#include "json.hpp"
#include "McpProtocol.h"
#include "McpSecurity.h"
#include "McpAuditLog.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace serialctl::mcp {

using Json = nlohmann::json;

class McpDispatcher {
public:
    using GuiExecutor = std::function<Json(const std::string& method, const Json& params)>;
    using GuiStatusChecker = std::function<bool()>;
    using GuiLauncher = std::function<bool(bool forceFocus, std::string& error)>;
    using ServiceConfigurator = std::function<Json(const Json& params)>;
    using ServiceStatusProvider = std::function<Json()>;
    using PowerContextProvider = std::function<void(bool& testUnlocked, Json& selectedChannels)>;

    McpDispatcher();

    void SetGuiExecutor(GuiExecutor executor) { guiExecutor_ = std::move(executor); }
    void SetGuiStatusChecker(GuiStatusChecker checker) { guiStatusChecker_ = std::move(checker); }
    void SetGuiLauncher(GuiLauncher launcher) { guiLauncher_ = std::move(launcher); }
    void SetServiceConfigurator(ServiceConfigurator configurator) { serviceConfigurator_ = std::move(configurator); }
    void SetServiceStatusProvider(ServiceStatusProvider provider) { serviceStatusProvider_ = std::move(provider); }
    void SetPowerContextProvider(PowerContextProvider provider) { powerContextProvider_ = std::move(provider); }

    void SetTestUnlocked(bool unlocked) {
        std::lock_guard<std::mutex> lock(mutex_);
        testUnlocked_ = unlocked;
    }
    void SetGuiSelectedChannels(Json channels) {
        std::lock_guard<std::mutex> lock(mutex_);
        guiSelectedChannels_ = std::move(channels);
    }

    // Main JSON-RPC dispatch entrance
    std::string Dispatch(const std::string& clientIp, const std::string& jsonRpcRequest);

    // Single JSON-RPC request processor
    Json ProcessRequest(const std::string& clientIp, const Json& request);

    // Call a tool directly
    Json CallTool(const std::string& clientIp, const std::string& toolName, const Json& arguments);

private:
    Json HandleAppStatus();
    Json HandleAppHealth();
    Json HandleAppLaunch(const Json& arguments);
    Json HandleAppActivate();
    Json HandleAppQuit(const Json& arguments);
    Json HandleServiceStatus();
    Json HandleServiceConfigure(const Json& arguments);

    mutable std::mutex mutex_;
    GuiExecutor guiExecutor_;
    GuiStatusChecker guiStatusChecker_;
    GuiLauncher guiLauncher_;
    ServiceConfigurator serviceConfigurator_;
    ServiceStatusProvider serviceStatusProvider_;
    PowerContextProvider powerContextProvider_;

    bool testUnlocked_ = false;
    Json guiSelectedChannels_ = Json::array();
    uint64_t startTimeTick_ = 0;
};

} // namespace serialctl::mcp
