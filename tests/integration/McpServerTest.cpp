#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <aclapi.h>

#include "LocalWorkspace.h"
#include "McpProtocol.h"
#include "McpSecurity.h"
#include "McpDispatcher.h"
#include "McpHttpServer.h"
#include "McpService.h"
#include "PowerService.h"

namespace serialctl {
// The test-only seam is compiled exclusively into mcp_server_test. Production
// authentication and the public password-checking contract remain untouched.
struct PowerServiceTestAccess {
    static bool UnlockSimulation(PowerService& service) {
        std::lock_guard<std::mutex> lock(service.mutex_);
        if (service.stopping_) return false;
        service.lockRequested_ = false;
        PowerService::testEnabled_.store(true);
        return true;
    }
};
} // namespace serialctl

using namespace serialctl;
using namespace serialctl::mcp;

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "REQUIRE FAILED: " << message << std::endl;
        std::exit(1);
    }
}

std::string WideToUtf8(const std::wstring& str) {
    if (str.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string HttpPostWithOrigin(uint16_t port, const std::string& path, const std::string& body,
                              const std::string& origin, std::string& outHeaders) {
    outHeaders.clear();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int res = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (res == SOCKET_ERROR) {
        closesocket(s);
        return "";
    }

    std::string req = "POST " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!origin.empty()) {
        req += "Origin: " + origin + "\r\n";
    }
    req += "\r\n" + body;

    send(s, req.data(), static_cast<int>(req.size()), 0);

    std::string resp;
    char buf[4096];
    int n = 0;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, n);
    }
    closesocket(s);

    size_t pos = resp.find("\r\n\r\n");
    if (pos != std::string::npos) {
        outHeaders = resp.substr(0, pos);
        return resp.substr(pos + 4);
    }
    return resp;
}

std::string HttpPost(uint16_t port, const std::string& path, const std::string& body) {
    std::string headers;
    return HttpPostWithOrigin(port, path, body, "", headers);
}

std::string HttpGet(uint16_t port, const std::string& path) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int res = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (res == SOCKET_ERROR) {
        closesocket(s);
        return "";
    }

    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";

    send(s, req.data(), static_cast<int>(req.size()), 0);

    std::string resp;
    char buf[4096];
    int n = 0;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, n);
    }
    closesocket(s);

    size_t pos = resp.find("\r\n\r\n");
    if (pos != std::string::npos) return resp.substr(pos + 4);
    return resp;
}

// -------------------------------------------------------------
// Test 1: MCP JSON-RPC protocol & Tool Discovery
// -------------------------------------------------------------
void TestProtocolAndDiscovery() {
    std::cout << "[Test 1] Testing MCP JSON-RPC protocol & Tool Discovery..." << std::endl;

    McpDispatcher dispatcher;
    McpHttpServer server;
    std::wstring err;
    const uint16_t testPort = 7181;

    bool ok = server.Start("127.0.0.1", testPort,
        [&dispatcher](const std::string& ip, const std::string& body) {
            return dispatcher.Dispatch(ip, body);
        },
        [&dispatcher]() {
            return dispatcher.CallTool("127.0.0.1", "app.health", Json::object()).dump();
        },
        err);
    Expect(ok, "McpHttpServer should start successfully");

    // 1. initialize
    Json initReq = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", Json::object()}
    };
    std::string initRespStr = HttpPost(testPort, "/mcp", initReq.dump());
    Expect(!initRespStr.empty(), "initialize response must not be empty");
    Json initResp = Json::parse(initRespStr);
    Expect(initResp["jsonrpc"] == "2.0", "JSON-RPC version must be 2.0");
    Expect(initResp["id"] == 1, "ID must match");
    Expect(initResp["result"]["protocolVersion"] == "2025-03-26", "protocolVersion must be 2025-03-26");
    Expect(initResp["result"]["serverInfo"]["name"] == "serialctl-mcp", "Server name must match");
    Expect(initResp["result"]["capabilities"].contains("tools"), "Must declare tools capability");

    // Stateless Streamable HTTP notification: 202 and no response body.
    std::string notificationHeaders;
    const std::string notificationBody = HttpPostWithOrigin(
        testPort, "/mcp", R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
        "", notificationHeaders);
    Expect(notificationHeaders.find("202 Accepted") != std::string::npos,
           "initialized notification must return HTTP 202 Accepted");
    Expect(notificationBody.empty(), "accepted notification must have no JSON body");
    // Reject a missing JSON-RPC id rather than executing an untracked tool call.
    std::string badHeaders;
    HttpPostWithOrigin(testPort, "/mcp", R"({"jsonrpc":"2.0","method":"tools/call","params":{}})",
                       "", badHeaders);
    Expect(badHeaders.find("400 Bad Request") != std::string::npos,
           "untracked tools/call notification must not execute");

    // 2. ping
    Json pingReq = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}};
    std::string pingRespStr = HttpPost(testPort, "/mcp", pingReq.dump());
    Json pingResp = Json::parse(pingRespStr);
    Expect(pingResp["id"] == 2 && pingResp["result"].is_object(), "ping must return empty object");

    // 3. tools/list
    Json toolsReq = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}};
    std::string toolsRespStr = HttpPost(testPort, "/mcp", toolsReq.dump());
    Json toolsResp = Json::parse(toolsRespStr);
    Expect(toolsResp["id"] == 3, "tools/list ID must match");
    Expect(toolsResp["result"]["tools"].is_array(), "tools must be an array");

    const auto& toolsList = toolsResp["result"]["tools"];
    Expect(toolsList.size() >= 80, "tools/list must contain at least 80 tools");

    std::vector<std::string> toolNames;
    for (const auto& t : toolsList) {
        toolNames.push_back(t["name"].get<std::string>());
        Expect(!t["description"].get<std::string>().empty(), "Every tool must have a description");
        Expect(t["inputSchema"]["type"] == "object", "Every tool must have an object inputSchema");
    }

    auto hasTool = [&](const std::string& name) {
        return std::find(toolNames.begin(), toolNames.end(), name) != toolNames.end();
    };
    Expect(hasTool("app.status"), "Must contain app.status");
    Expect(hasTool("app.health"), "Must contain app.health");
    Expect(hasTool("app.launch"), "Must contain app.launch");
    Expect(hasTool("session.list"), "Must contain session.list");
    Expect(hasTool("session.connect_serial"), "Must contain session.connect_serial");
    Expect(hasTool("session.connect_ssh"), "Must contain session.connect_ssh");
    Expect(hasTool("ssh_history.list"), "Must contain ssh_history.list");
    Expect(hasTool("terminal.read"), "Must contain terminal.read");
    Expect(hasTool("terminal.command"), "Must contain terminal.command");
    Expect(hasTool("boot.enter"), "Must contain boot.enter");
    Expect(hasTool("macro.list"), "Must contain macro.list");
    Expect(hasTool("sftp.list"), "Must contain sftp.list");
    Expect(hasTool("sftp.chmod"), "Must contain sftp.chmod");
    Expect(hasTool("xfer.start"), "Must contain xfer.start");
    Expect(hasTool("tftp.set_directory"), "Must contain tftp.set_directory");
    Expect(hasTool("tftp.set_bind_ip"), "Must contain tftp.set_bind_ip");
    Expect(hasTool("tftp.publish_local"), "Must contain tftp.publish_local");
    Expect(hasTool("power.get"), "Must contain power.get");
    Expect(hasTool("power.output"), "Must contain power.output");
    Expect(hasTool("power.action"), "Must contain power.action");
    Expect(hasTool("power.recover"), "Must contain power.recover");
    Expect(hasTool("log.export"), "Must contain log.export");
    Expect(hasTool("ui.theme"), "Must contain ui.theme");
    Expect(hasTool("ai.send"), "Must contain ai.send");

    // B03: Verify sftp.chmod with mode: 493 is NOT blocked by power blacklist
    dispatcher.SetGuiStatusChecker([] { return true; });
    dispatcher.SetGuiExecutor([](const std::string& method, const Json& params) -> Json {
        if (method == "sftp.chmod") {
            return {{"ok", true}, {"mode", params.value("mode", 0)}};
        }
        return {{"ok", true}};
    });
    Json chmodRes = dispatcher.CallTool("127.0.0.1", "sftp.chmod", {{"path", "/tmp/run.sh"}, {"mode", 493}});
    Expect(!chmodRes.value("isError", false), "sftp.chmod with mode 493 must NOT be blocked by power security blacklist");

    // B09: Verify app.service_configure returns error when configurator not bound, and succeeds when bound
    Json unconfigured = dispatcher.CallTool("127.0.0.1", "app.service_configure", {{"port", 7088}});
    Expect(unconfigured.contains("error") && unconfigured["error"]["code"] == ErrInternalError,
           "app.service_configure without configurator must return ErrInternalError");

    bool configuredCalled = false;
    dispatcher.SetServiceConfigurator([&configuredCalled](const Json& p) -> Json {
        configuredCalled = true;
        return {{"ok", true}, {"port", p.value("port", 0)}};
    });
    Json configuredRes = dispatcher.CallTool("127.0.0.1", "app.service_configure", {{"port", 7088}});
    Expect(!configuredRes.value("isError", false), "app.service_configure with bound configurator must succeed");
    Expect(configuredCalled, "Configurator callback must be executed");

    // 4. GET /health
    std::string healthRespStr = HttpGet(testPort, "/health");
    Expect(!healthRespStr.empty(), "GET /health must succeed");
    Json healthResp = Json::parse(healthRespStr);
    Expect(healthResp["content"][0]["type"] == "text", "health must return content text");

    server.Stop();
    std::cout << "[Test 1] PASS: All MCP protocol methods, tools discovery (" << toolsList.size()
              << " tools), and unauthenticated access verified." << std::endl;
}

// -------------------------------------------------------------
// Test 2: Power Whitelist Positive & Hard Blacklist Rejections
// -------------------------------------------------------------
void TestPowerWhitelistAndRejections() {
    std::cout << "[Test 2] Testing Power Whitelist and Hard Blacklist Rejections..." << std::endl;

    PowerService power;
    bool authOk = PowerServiceTestAccess::UnlockSimulation(power);
    Expect(authOk && PowerService::ConnectionAvailable(), "Test unlock must succeed with valid password");

    // Connect simulation backend
    auto connectAction = power.Submit({{"type", "connect"}, {"backend", "simulation"}});
    for (int i = 0; i < 50; ++i) {
        if (power.Action(connectAction.at("id")).value("state", "") == "completed") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Expect(power.State().value("connected", false), "Power supply simulation must connect");

    McpDispatcher dispatcher;
    dispatcher.SetTestUnlocked(true);
    dispatcher.SetGuiSelectedChannels(Json::array({1})); // Channel 1 selected in GUI

    dispatcher.SetGuiStatusChecker([] { return true; });

    // B02: Real E2E contract bridging simulation mimicking MainWindow::McpExecute
    uint64_t mcpSeq = 0;
    dispatcher.SetGuiExecutor([&power, &mcpSeq](const std::string& method, const Json& params) -> Json {
        if (method == "power.get") {
            return power.State();
        }
        if (method == "power.output") {
            if (!PowerService::ConnectionAvailable()) {
                return {{"error", "Power supply operations require local test unlock"}};
            }
            auto state = power.State();
            if (!state.value("connected", false)) {
                return {{"error", "Power supply is not connected"}};
            }
            Json effectiveParams = params;
            // Inject protected state
            effectiveParams["channels"] = Json::array({1});
            effectiveParams["identity"] = state.value("identity", std::string());
            effectiveParams["mode"] = state.value("mode", std::string());
            effectiveParams["requestId"] = "mcp-" + std::to_string(++mcpSeq);

            // Submit with required contract keys
            return power.Submit({
                {"type", "output"},
                {"source", "ai:desktop"},
                {"channels", effectiveParams["channels"]},
                {"enabled", effectiveParams["enabled"]},
                {"requestId", effectiveParams["requestId"]}
            });
        }
        if (method == "power.action") {
            return power.Action(params.at("id").get<std::string>());
        }
        if (method == "boot.enter") {
            return {{"ok", true}, {"preset", params.value("preset", "")}};
        }
        return {{"error", "Unknown"}};
    });

    // 1. Positive: power.get
    Json getResult = dispatcher.CallTool("127.0.0.1", "power.get", Json::object());
    Expect(getResult.contains("content"), "power.get must return content");
    Expect(!getResult.value("isError", false), "power.get must not be error");

    // 2. Positive: power.output enabled=true on checked channel 1 (contract bridged)
    Json outResult = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"reason", "Test energize"}
    });
    Expect(outResult.contains("content"), "power.output must return content");
    Expect(!outResult.value("isError", false), "power.output on selected channels must succeed");

    // 3. Locked Test rejection: when test is locked, power.output and boot.enter(autoPowerCycle=true) must be rejected
    power.DisableTest();
    dispatcher.SetTestUnlocked(false);
    Json lockedOut = dispatcher.CallTool("127.0.0.1", "power.output", {{"enabled", true}});
    Expect(lockedOut.contains("error"), "power.output must be rejected when test is locked");
    Json bootNoPower = dispatcher.CallTool("127.0.0.1", "boot.enter", {{"preset", "U-Boot (stop)"}});
    Expect(!bootNoPower.contains("error"), "boot.enter without autoPowerCycle must not be blocked by power security");
    Json bootWithPowerLocked = dispatcher.CallTool("127.0.0.1", "boot.enter", {{"preset", "U-Boot (stop)"}, {"autoPowerCycle", true}});
    Expect(bootWithPowerLocked.contains("error") && bootWithPowerLocked["error"]["code"] == ErrPermissionDenied,
           "boot.enter with autoPowerCycle=true must be rejected when test is locked");
    // Restore unlocked state
    Expect(PowerServiceTestAccess::UnlockSimulation(power), "Simulation unlock must be restored");
    dispatcher.SetTestUnlocked(true);

    // 4. Negative: Attempt to set voltage -> Hard rejection
    Json voltCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"voltage", 5.0}
    });
    Expect(voltCall.contains("error"), "voltage modification must be rejected");
    Expect(voltCall["error"]["code"] == ErrPowerParameterForbidden, "Must return ErrPowerParameterForbidden");
    Expect(voltCall["error"]["message"].get<std::string>().find("POWER_PARAMETER_CHANGE_FORBIDDEN") != std::string::npos,
           "Must contain POWER_PARAMETER_CHANGE_FORBIDDEN in message");

    // 5. Negative: Attempt to set current -> Hard rejection
    Json currCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"current", 2.0}
    });
    Expect(currCall["error"]["code"] == ErrPowerParameterForbidden, "current modification must be rejected");

    // 6. Negative: Attempt to set OVP / OCP -> Hard rejection
    Json ovpCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"ovp", 6.5}
    });
    Expect(ovpCall["error"]["code"] == ErrPowerParameterForbidden, "ovp modification must be rejected");

    // 7. Negative: Attempt to set mode (series/parallel) -> Hard rejection
    Json modeCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"mode", "parallel"}
    });
    Expect(modeCall["error"]["code"] == ErrPowerParameterForbidden, "mode modification must be rejected");

    // 8. Negative: Attempt to override channels (tamper GUI selection) -> Hard rejection
    Json chanCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"channels", Json::array({1, 2, 3})} // GUI has only [1]
    });
    Expect(chanCall["error"]["code"] == ErrPowerParameterForbidden, "channel tampering must be rejected");

    // 9. Negative: Attempt to send raw SCPI -> Hard rejection
    Json scpiCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"scpi", "*IDN?\nVOLT 10"}
    });
    Expect(scpiCall["error"]["code"] == ErrPowerParameterForbidden, "SCPI modification must be rejected");

    // 10. Negative: Calling unexposed tools -> Hard rejection
    for (const char* unexposed : {"power.set", "power.calibrate", "power.reset", "power.preset", "power.voltage"}) {
        Json unexpCall = dispatcher.CallTool("127.0.0.1", unexposed, Json::object());
        Expect(unexpCall.contains("error") && unexpCall["error"]["code"] == ErrPowerParameterForbidden,
               "Unexposed power tools must be hard rejected");
    }

    // 11. Verify zero side-effects on power supply state
    auto finalState = power.State();
    Expect(finalState.value("connected", false), "Power supply remains connected");
    Expect(finalState["channels"][0]["setVoltage"].get<double>() < 1.0, "Voltage must remain unmodified");

    std::cout << "[Test 2] PASS: Power whitelist and 100% hard blacklist rejection verified with zero side-effects." << std::endl;
}

// -------------------------------------------------------------
// Test 3: Business Full-Flow & Password Sanitization
// -------------------------------------------------------------
void TestBusinessFlowAndPasswordMasking() {
    std::cout << "[Test 3] Testing Business Full-Flow, Password Masking & AI Recursion..." << std::endl;

    McpDispatcher dispatcher;
    dispatcher.SetGuiStatusChecker([] { return true; });

    Json mockMacros = Json::array();
    dispatcher.SetGuiExecutor([&mockMacros](const std::string& method, const Json& params) -> Json {
        if (method == "session.list") {
            return Json::array({
                {{"id", "session-1"}, {"name", "[COM] COM3"}, {"mode", 1}, {"connected", true}}
            });
        }
        if (method == "ssh_history.list") {
            // Raw internal state with passwords
            return Json::array({
                {
                    {"host", "192.168.1.100"},
                    {"username", "root"},
                    {"port", 22},
                    {"password", "plaintext_secret_password_123"},
                    {"secretKey", "private_ssh_key_secret"}
                }
            });
        }
        if (method == "macro.add") {
            mockMacros.push_back(params);
            return {{"ok", true}, {"index", mockMacros.size() - 1}};
        }
        if (method == "macro.list") {
            return {{"macros", mockMacros}};
        }
        if (method == "tftp.status") {
            return {{"ok", true}, {"listening", true}, {"port", 69}};
        }
        if (method == "ai.send") {
            return {{"ok", true}, {"promptReceived", params.value("prompt", "")}};
        }
        return {{"ok", true}};
    });

    // 1. Session list
    Json sessionRes = dispatcher.CallTool("127.0.0.1", "session.list", Json::object());
    Expect(!sessionRes.value("isError", false), "session.list must succeed");

    // 2. Macro management
    Json addRes = dispatcher.CallTool("127.0.0.1", "macro.add", {
        {"name", "Test Step"},
        {"commands", Json::array({"uname -a", "ls -la"})}
    });
    Expect(!addRes.value("isError", false), "macro.add must succeed");

    Json listRes = dispatcher.CallTool("127.0.0.1", "macro.list", Json::object());
    Expect(!listRes.value("isError", false), "macro.list must succeed");

    // 3. TFTP status
    Json tftpRes = dispatcher.CallTool("127.0.0.1", "tftp.status", Json::object());
    Expect(!tftpRes.value("isError", false), "tftp.status must succeed");

    // 4. SSH History Password Masking Check
    Json historyRes = dispatcher.CallTool("127.0.0.1", "ssh_history.list", Json::object());
    Expect(!historyRes.value("isError", false), "ssh_history.list must succeed");
    std::string historyStr = historyRes["content"][0]["text"].get<std::string>();
    Expect(historyStr.find("plaintext_secret_password_123") == std::string::npos,
           "Plaintext password must NEVER appear in ssh_history.list response!");
    Expect(historyStr.find("[PROTECTED]") != std::string::npos,
           "Password field must be masked with [PROTECTED]");

    // Verify Audit log also does not leak password
    Json auditLog = McpAuditLog::Instance().ToJson();
    std::string auditStr = auditLog.dump();
    Expect(auditStr.find("plaintext_secret_password_123") == std::string::npos,
           "Plaintext password must NEVER appear in McpAuditLog!");

    // 5. AI Recursion Guard
    Json aiAllowed = dispatcher.CallTool("127.0.0.1", "ai.send", {
        {"prompt", "hello"},
        {"recursionDepth", 1}
    });
    Expect(!aiAllowed.value("isError", false), "ai.send with recursionDepth 1 must succeed");

    Json aiRejected = dispatcher.CallTool("127.0.0.1", "ai.send", {
        {"prompt", "nested loop"},
        {"recursionDepth", 2}
    });
    Expect(aiRejected.contains("error") && aiRejected["error"]["code"] == ErrAiRecursionLimit,
           "ai.send with recursionDepth >= 2 must be rejected with ErrAiRecursionLimit");

    std::cout << "[Test 3] PASS: Business full-flow, password masking, and AI recursion limit verified." << std::endl;
}

// -------------------------------------------------------------
// Test 4: Resident Service Lifecycle, GUI Offline & Launch
// -------------------------------------------------------------
void TestResidentLifecycle() {
    std::cout << "[Test 4] Testing Resident Service Lifecycle, GUI Offline & Idempotent Launch..." << std::endl;

    McpDispatcher dispatcher;
    bool guiRunning = false;
    std::atomic<int> launchCount{0};

    dispatcher.SetGuiStatusChecker([&guiRunning] { return guiRunning; });
    dispatcher.SetGuiLauncher([&guiRunning, &launchCount](bool, std::string&) {
        launchCount++;
        guiRunning = true;
        return true;
    });
    dispatcher.SetGuiExecutor([](const std::string&, const Json&) -> Json {
        return {{"ok", true}};
    });

    // 1. When GUI is offline
    guiRunning = false;

    // app.health still returns ok and serviceRunning=true, guiRunning=false
    Json healthRes = dispatcher.CallTool("127.0.0.1", "app.health", Json::object());
    Json healthBody = Json::parse(healthRes["content"][0]["text"].get<std::string>());
    Expect(healthBody["serviceRunning"] == true, "Service must be running when GUI is offline");
    Expect(healthBody["guiRunning"] == false, "GUI must be reported as offline");

    // Device tools must return APP_NOT_RUNNING error
    Json sessionRes = dispatcher.CallTool("127.0.0.1", "session.list", Json::object());
    Expect(sessionRes.contains("error") && sessionRes["error"]["code"] == ErrAppNotRunning,
           "Calling device tools when GUI is offline must return ErrAppNotRunning (-32002)");

    // B08: Verify McpService has offline TFTP server instance and serves tftp.status / tftp.list when GUI is offline
    TftpServer& srvTftp = McpService::Instance().Tftp();
    Expect(srvTftp.State() == "stopped" || srvTftp.State() == "listening", "Offline TFTP server must be valid in McpService");
    if (!McpService::Instance().IsGuiRunning()) {
        Json offlineTftpStatus = McpService::Instance().Dispatcher().CallTool("127.0.0.1", "tftp.status", Json::object());
        Expect(!offlineTftpStatus.contains("error") && !offlineTftpStatus.value("isError", false),
               "tftp.status must succeed via McpService offline TFTP handler when GUI is offline");
        Json offlineTftpRemote = McpService::Instance().Dispatcher().CallTool("127.0.0.1", "tftp.publish_remote",
                                                                              {{"tftpName", "Image"}, {"remotePath", "/boot/Image"}});
        Expect(offlineTftpRemote.contains("error") && offlineTftpRemote["error"]["code"] == ErrAppNotRunning,
               "tftp.publish_remote must require GUI when GUI is offline");

        namespace fs = std::filesystem;
        const fs::path tempRoot = fs::temp_directory_path() /
            (L"serialctl_mcp_tftp_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(tempRoot, ec);
        Expect(fs::create_directories(tempRoot / L"subdir", ec), "Create MCP TFTP test root directory");
        {
            std::ofstream f1(tempRoot / L"uImage", std::ios::binary);
            f1 << "uimage-sample";
            std::ofstream f2(tempRoot / L"rootfs.cpio", std::ios::binary);
            f2 << "rootfs-sample";
            std::ofstream f3(tempRoot / L"subdir" / L"ignored.bin", std::ios::binary);
            f3 << "subdir-ignored";
        }
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        const fs::path stateIniPath = fs::path(exePath).parent_path() / L"state.ini";
        const bool hadStateIni = fs::exists(stateIniPath, ec);
        std::string priorStateIni;
        if (hadStateIni) {
            std::ifstream in(stateIniPath, std::ios::binary);
            priorStateIni.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        struct OfflineTftpGuard {
            fs::path root;
            TftpServer& srv;
            fs::path stateIni;
            bool hadIni;
            std::string priorIni;
            ~OfflineTftpGuard() {
                srv.ClearRoot();
                std::error_code cleanupEc;
                fs::remove_all(root, cleanupEc);
                if (hadIni) {
                    std::ofstream out(stateIni, std::ios::binary | std::ios::trunc);
                    out.write(priorIni.data(), static_cast<std::streamsize>(priorIni.size()));
                } else {
                    fs::remove(stateIni, cleanupEc);
                }
            }
        } tftpGuard{tempRoot, srvTftp, stateIniPath, hadStateIni, std::move(priorStateIni)};

        const std::string rootUtf8 = WideToUtf8(tempRoot.wstring());
        Json setDirRes = McpService::Instance().Dispatcher().CallTool(
            "192.168.31.50", "tftp.set_directory", {{"directory", rootUtf8}});
        Expect(!setDirRes.contains("error") && !setDirRes.value("isError", false),
               "Remote LAN MCP client must be able to call tftp.set_directory");
        Json setDirBody = Json::parse(setDirRes["content"][0]["text"].get<std::string>());
        Expect(setDirBody.value("ok", false) && setDirBody.value("fileCount", 0) == 2 &&
                   setDirBody["files"].is_array() && setDirBody["files"].size() == 2,
               "tftp.set_directory must return 2 regular files");

        Json statusAfter = McpService::Instance().Dispatcher().CallTool("192.168.31.50", "tftp.status", Json::object());
        Expect(!statusAfter.contains("error") && !statusAfter.value("isError", false),
               "tftp.status must succeed from LAN client");
        Json statusBody = Json::parse(statusAfter["content"][0]["text"].get<std::string>());
        Expect(statusBody.value("ok", false) && !statusBody.value("directory", std::string()).empty() &&
                   !statusBody.value("defaultIp", std::string()).empty() &&
                   statusBody.contains("serverIps") && statusBody["serverIps"].is_array() &&
                   !statusBody["serverIps"].empty() && statusBody.value("fileCount", 0) == 2,
               "tftp.status must expose directory, defaultIp, serverIps, and fileCount");

        Json listRes = McpService::Instance().Dispatcher().CallTool("192.168.31.50", "tftp.list", Json::object());
        Expect(!listRes.contains("error") && !listRes.value("isError", false),
               "tftp.list must succeed from LAN client");
        Json listBody = Json::parse(listRes["content"][0]["text"].get<std::string>());
        Expect(listBody.value("mode", std::string()) == "directory" &&
                   listBody["files"].is_array() && listBody["files"].size() == 2,
               "tftp.list must return directory files");
        const std::string listDump = listBody["files"].dump();
        Expect(listDump.find("uImage") != std::string::npos &&
                   listDump.find("rootfs.cpio") != std::string::npos &&
                   listDump.find("ignored.bin") == std::string::npos,
               "tftp.list must include root files and exclude nested subdirectory files");

        Json setIpRes = McpService::Instance().Dispatcher().CallTool(
            "192.168.31.50", "tftp.set_bind_ip", {{"bindIp", "127.0.0.1"}});
        Expect(!setIpRes.contains("error") && !setIpRes.value("isError", false),
               "Remote LAN MCP client must be able to call tftp.set_bind_ip");
        Json setIpBody = Json::parse(setIpRes["content"][0]["text"].get<std::string>());
        Expect(setIpBody.value("ok", false) && setIpBody.value("bindIp", std::string()) == "127.0.0.1",
               "tftp.set_bind_ip must update bindIp to 127.0.0.1");

        Json badIpRes = McpService::Instance().Dispatcher().CallTool(
            "192.168.31.50", "tftp.set_bind_ip", {{"bindIp", "203.0.113.99"}});
        Expect(badIpRes.contains("error") || badIpRes.value("isError", false),
               "tftp.set_bind_ip must reject non-local IPv4 addresses");
    }

    // 2. Simulate Session 0 / no interactive login
    McpService::SetSimulateSession0ForTest(true);
    dispatcher.SetGuiLauncher([](bool, std::string& err) {
        err = "LOGIN_REQUIRED: Interactive logon session required (Session 0 detected)";
        return false;
    });

    Json session0Launch = dispatcher.CallTool("127.0.0.1", "app.launch", Json::object());
    Expect(session0Launch.contains("error") && session0Launch["error"]["code"] == ErrLoginRequired,
           "app.launch in Session 0 must return LOGIN_REQUIRED (-32003)");
    McpService::SetSimulateSession0ForTest(false);

    // 3. Simulate interactive session: Concurrent 10 app.launch calls must be idempotent
    dispatcher.SetGuiLauncher([&guiRunning, &launchCount](bool, std::string&) {
        static std::mutex s_mtx;
        std::lock_guard<std::mutex> lk(s_mtx);
        if (!guiRunning) {
            launchCount++;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            guiRunning = true;
        }
        return true;
    });

    std::vector<std::thread> launchThreads;
    for (int i = 0; i < 10; ++i) {
        launchThreads.emplace_back([&dispatcher] {
            dispatcher.CallTool("127.0.0.1", "app.launch", Json::object());
        });
    }
    for (auto& t : launchThreads) {
        t.join();
    }

    Expect(launchCount.load() == 1, "Concurrent app.launch calls must be idempotent (launchCount == 1)");
    Expect(guiRunning == true, "GUI must now be running");

    // 4. Device tools become available once GUI is launched
    Json sessionResAfter = dispatcher.CallTool("127.0.0.1", "session.list", Json::object());
    Expect(!sessionResAfter.value("isError", false), "Device tools must succeed after GUI is running");

    // 5. Test service stop closes HTTP port & B12 CORS lock-down
    McpHttpServer httpSrv;
    std::wstring err;
    const uint16_t testPort = 7182;
    std::atomic<int> executedHttpCalls{0};
    const bool listenerStarted = httpSrv.Start("127.0.0.1", testPort,
        [&dispatcher, &executedHttpCalls](const std::string& ip, const std::string& body) {
            ++executedHttpCalls;
            return dispatcher.Dispatch(ip, body);
        },
        [] { return "{\"status\":\"ok\"}"; },
        err);
    Expect(listenerStarted, "HTTP listener must start");

    std::string testPing = HttpGet(testPort, "/health");
    Expect(!testPing.empty(), "Port must be open while server is running");

    // Test CORS: Localhost origin is allowed
    std::string headersLocal;
    HttpPostWithOrigin(testPort, "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"ping\"}",
                       "http://localhost:3000", headersLocal);
    Expect(headersLocal.find("Access-Control-Allow-Origin: http://localhost:3000") != std::string::npos,
           "Localhost origin must be permitted in CORS");

    // A rejected browser Origin must not reach the MCP dispatcher at all.
    const int acceptedCount = executedHttpCalls.load();
    std::string headersEvil;
    HttpPostWithOrigin(testPort, "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"ping\"}",
                       "https://evil-attacker.com", headersEvil);
    Expect(headersEvil.find("403 Forbidden") != std::string::npos, "Cross-origin POST must be rejected");
    Expect(executedHttpCalls.load() == acceptedCount, "Cross-origin POST must not execute a tool");
    std::string headersSpoof;
    HttpPostWithOrigin(testPort, "/mcp", "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"ping\"}",
                       "http://localhost.attacker.example", headersSpoof);
    Expect(headersSpoof.find("403 Forbidden") != std::string::npos, "Spoofed localhost prefix must be rejected");
    Expect(executedHttpCalls.load() == acceptedCount, "Spoofed origin must not execute a tool");

    httpSrv.Stop();

    std::string testPingAfter = HttpGet(testPort, "/health");
    Expect(testPingAfter.empty(), "Port must be closed after server is stopped");

    // 6. Test Named Pipe IPC bridge and dynamic PowerContextProvider sync
    const std::wstring testPipe = L"\\\\.\\pipe\\SerialCtl.McpBridge.Test." + std::to_wstring(GetCurrentProcessId());
    McpIpcServer ipcServer(testPipe);
    bool ipcStarted = ipcServer.Start([](const std::string& reqStr) -> std::string {
        Json req = Json::parse(reqStr);
        std::string method = req.value("method", "");
        if (method == "internal.power_context") {
            return Json{{"testUnlocked", true}, {"selectedChannels", Json::array({2})}}.dump();
        }
        if (method == "power.output") {
            return Json{{"ok", true}, {"channels", req["params"]["channels"]}}.dump();
        }
        return Json{{"ok", true}, {"echoMethod", method}}.dump();
    });
    Expect(ipcStarted, "McpIpcServer must start");
    for (int i = 0; i < 50 && !WaitNamedPipeW(testPipe.c_str(), 20); ++i) {
        Sleep(10);
    }

    McpDispatcher ipcDispatcher;
    ipcDispatcher.SetGuiStatusChecker([&testPipe] {
        return WaitNamedPipeW(testPipe.c_str(), 100) != FALSE || GetLastError() == ERROR_PIPE_BUSY;
    });
    ipcDispatcher.SetPowerContextProvider([&testPipe](bool& unlocked, Json& channels) {
        std::string resp;
        if (SendIpcRequest(testPipe, "{\"method\":\"internal.power_context\"}", resp, 1000)) {
            Json j = Json::parse(resp);
            unlocked = j.value("testUnlocked", false);
            channels = j.value("selectedChannels", Json::array());
        }
    });
    ipcDispatcher.SetGuiExecutor([&testPipe](const std::string& method, const Json& params) -> Json {
        std::string resp;
        if (!SendIpcRequest(testPipe, Json{{"method", method}, {"params", params}}.dump(), resp, 1000)) {
            return {{"error", "IPC failed"}};
        }
        return Json::parse(resp);
    });

    Json ipcPowerOut = ipcDispatcher.CallTool("127.0.0.1", "power.output", {{"enabled", true}});
    Expect(!ipcPowerOut.value("isError", false), "IPC power.output with synced context must succeed");
    Json ipcOutBody = Json::parse(ipcPowerOut["content"][0]["text"].get<std::string>());
    Expect(ipcOutBody["channels"] == Json::array({2}), "PowerContextProvider must dynamically sync GUI selected channel [2]");

    Json ipcTamper = ipcDispatcher.CallTool("127.0.0.1", "power.output", {{"enabled", true}, {"channels", Json::array({1})}});
    Expect(ipcTamper.contains("error") && ipcTamper["error"]["code"] == ErrPowerParameterForbidden,
           "Tampering channel [1] when GUI has [2] must be rejected over IPC");

    ipcServer.Stop();

    std::cout << "[Test 4] PASS: Resident lifecycle, Session 0 isolation, IPC bridge, and idempotent launch verified." << std::endl;
}

std::string HttpRaw(uint16_t port, const std::string& request) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    Expect(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Raw HTTP connect");
    DWORD timeout = 5000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    Expect(send(socket, request.data(), static_cast<int>(request.size()), 0) == static_cast<int>(request.size()), "Raw HTTP send");
    shutdown(socket, SD_SEND);
    std::string response; char data[4096]; int size;
    while ((size = recv(socket, data, sizeof(data), 0)) > 0) response.append(data, size);
    closesocket(socket);
    return response;
}

void TestIpcAccessAndCancellation() {
    const auto name = L"\\\\.\\pipe\\SerialCtl.SecurityTest." + std::to_wstring(GetCurrentProcessId());
    McpIpcServer server(name);
    Expect(server.Start([](const std::string&) { return std::string("reply"); }), "IPC security server starts");
    std::string reply;
    Expect(SendIpcRequest(name, "request", reply, 3000) && reply == "reply", "Same-user IPC round trip");
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto deadline = GetTickCount64() + 3000;
    while (pipe == INVALID_HANDLE_VALUE && GetTickCount64() < deadline) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) Sleep(10);
    }
    Expect(pipe != INVALID_HANDLE_VALUE, "IPC pipe opens for its current user");
    PACL dacl = nullptr; PSECURITY_DESCRIPTOR security = nullptr;
    Expect(GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &security) == ERROR_SUCCESS,
           "IPC actual DACL must be readable");
    Expect(dacl && dacl->AceCount == 2, "IPC DACL must contain only current user and LocalSystem");
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* ace = nullptr; Expect(GetAce(dacl, index, &ace) != FALSE, "Read IPC ACE");
        auto allowed = reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace);
        PSID sid = reinterpret_cast<PSID>(&allowed->SidStart);
        Expect(!IsWellKnownSid(sid, WinInteractiveSid) && !IsWellKnownSid(sid, WinWorldSid) &&
               !IsWellKnownSid(sid, WinBuiltinAdministratorsSid), "IPC grants an unrelated group access");
    }
    LocalFree(security);
    // A client that never sends a complete request cannot prevent shutdown.
    const auto start = GetTickCount64();
    server.Stop();
    Expect(GetTickCount64() - start < 1000, "IPC stop blocked on stalled peer");
    CloseHandle(pipe);
    std::cout << "PASS: actual IPC DACL and cancellation with a stalled client\n";
}

void TestStatelessHttpResponseIntegrity() {
    std::cout << "[Test 5] Testing large stateless MCP JSON responses and input validation..." << std::endl;
    McpHttpServer server;
    std::wstring err;
    constexpr uint16_t testPort = 7189;
    const Json body = {{"jsonrpc", "2.0"}, {"id", 1},
                       {"result", {{"payload", std::string(512 * 1024, 'X')}}}};
    const std::string expected = body.dump();
    const bool started = server.Start("127.0.0.1", testPort,
        [&expected](const std::string&, const std::string&) { return expected; },
        [] { return std::string("{\"status\":\"ok\"}"); }, err);
    Expect(started, "Large MCP HTTP response server should start");
    const std::string actual = HttpPost(testPort, "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    Expect(actual == expected, "Entire HTTP body must arrive without partial send truncation");

    std::string headers;
    HttpPostWithOrigin(testPort, "/mcp",
        R"({"jsonrpc":42,"id":1,"method":"ping"})", "", headers);
    Expect(headers.find("400 Bad Request") != std::string::npos,
           "JSON-RPC with numeric protocol marker must return 400 instead of crashing");
    // The HTTP service must continue accepting good messages after an invalid request.
    Expect(HttpPost(testPort, "/mcp",
        R"({"jsonrpc":"2.0","id":2,"method":"ping"})") == expected,
        "HTTP request worker survives malformed JSON-RPC type");
    const std::string message = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    for (const std::string& extra : {
        std::string("Content-Length: 5\r\n"), std::string("Transfer-Encoding: chunked\r\n"),
        std::string("Origin: http://attacker.invalid\r\nOrigin: http://localhost\r\n")}) {
        auto raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(message.size()) + "\r\n" + extra + "\r\n" + message;
        Expect(HttpRaw(testPort, raw).find("400 Bad Request") != std::string::npos, "Ambiguous HTTP framing must be rejected");
    }
    for (const std::string& length : {std::string("12junk"), std::string("-1"), std::string("16777217")}) {
        const auto raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " + length + "\r\n\r\n" + message;
        Expect(HttpRaw(testPort, raw).find("400 Bad Request") != std::string::npos, "Invalid Content-Length must not be parsed as a prefix");
    }
    Expect(HttpRaw(testPort, "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/jsonp\r\nContent-Length: " +
        std::to_string(message.size()) + "\r\n\r\n" + message).find("400 Bad Request") != std::string::npos,
        "JSONP content type must be rejected");
    server.Stop();
}

void TestWorkspaceLoopbackBoundary() {
    std::cout << "[Test 6] Testing Windows local workspace loopback boundary, ephemeral MCP port and HTTP exec..." << std::endl;
    wchar_t tmpBuf[MAX_PATH]{};
    const DWORD tmpLen = GetTempPathW(MAX_PATH, tmpBuf);
    Expect(tmpLen > 0 && tmpLen < MAX_PATH, "GetTempPathW failed in workspace test");
    const std::wstring rootDir =
        std::wstring(tmpBuf, tmpLen) + L"serialctl-mcptest-" + std::to_wstring(GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::remove_all(rootDir, ec);
    Expect(std::filesystem::create_directories(rootDir, ec), "Failed to create workspace HTTP test root");

    auto cleanup = [&] {
        std::error_code remEc;
        std::filesystem::remove_all(rootDir, remEc);
    };

    try {
        ai::LocalWorkspaceService ws;
        std::string cfgErr;
        Expect(ws.Configure(true, rootDir, false, cfgErr), "Failed to configure LocalWorkspaceService");

        McpDispatcher dispatcher;
        dispatcher.SetGuiStatusChecker([] { return true; });
        dispatcher.SetWorkspaceExecutor([&ws](const std::string& method, const Json& arguments) {
            return ws.ExecuteTool(method, arguments);
        });

        // 1. Non-loopback client IP MUST be denied (-32007 PERMISSION_DENIED) for every workspace.* tool.
        McpAuditLog::Instance().Clear();
        const Json lanBlocked = dispatcher.CallTool("192.168.1.50", "workspace.info", Json::object());
        Expect(lanBlocked.contains("error") && lanBlocked["error"]["code"] == ErrPermissionDenied,
               "Non-loopback workspace.info call must be blocked with -32007");
        const Json lanExecBlocked =
            dispatcher.CallTool("10.0.0.8", "workspace.exec", {{"command", "echo PWN"}});
        Expect(lanExecBlocked.contains("error") && lanExecBlocked["error"]["code"] == ErrPermissionDenied,
               "Non-loopback workspace.exec call must be blocked with -32007");

        // 2. Loopback ephemeral HTTP listener (allowEphemeralPort = true) serves workspace.* over /mcp
        McpHttpServer server;
        std::wstring err;
        Expect(server.Start(
                   "127.0.0.1", 0,
                   [&dispatcher](const std::string& ip, const std::string& body) {
                       return dispatcher.Dispatch(ip, body);
                   },
                   [] { return std::string("{\"status\":\"ok\"}"); },
                   err, true),
               "Ephemeral loopback MCP HTTP server failed to start");
        const uint16_t port = server.Port();
        Expect(port != 0, "Ephemeral MCP port must resolve to non-zero port");

        auto callHttpTool = [&](int id, const std::string& name, const Json& args) {
            const Json req = {{"jsonrpc", "2.0"},
                              {"id", id},
                              {"method", "tools/call"},
                              {"params", {{"name", name}, {"arguments", args}}}};
            const std::string raw = HttpPost(port, "/mcp", req.dump());
            Expect(!raw.empty(), "HTTP tools/call response empty for workspace tool");
            return Json::parse(raw);
        };

        const Json writeResp = callHttpTool(1, "workspace.write_file",
                                            {{"path", "hello.txt"}, {"content", "SECRET_BODY_XYZ"}});
        Expect(!writeResp["result"].value("isError", false), "HTTP workspace.write_file failed");
        const Json execResp = callHttpTool(2, "workspace.exec", {{"command", "echo MCP_WS_EXEC_OK"}});
        const auto execJson = Json::parse(execResp["result"]["content"][0]["text"].get<std::string>());
        Expect(execJson["ok"] == true && execJson["exitCode"] == 0 &&
                   execJson["stdout"].get<std::string>().find("MCP_WS_EXEC_OK") != std::string::npos,
               "HTTP workspace.exec output mismatch");

        // Audit log must redact large file content on workspace.write_file
        const Json audit = McpAuditLog::Instance().ToJson();
        bool sawSanitizedWrite = false;
        for (const auto& entry : audit) {
            if (entry.value("toolName", "") == "workspace.write_file") {
                const auto contentVal = entry["arguments"].value("content", std::string());
                Expect(contentVal.find("SECRET_BODY_XYZ") == std::string::npos &&
                           contentVal.rfind("[TEXT:", 0) == 0,
                       "Audit log leaked raw workspace.write_file file body");
                sawSanitizedWrite = true;
            }
        }
        Expect(sawSanitizedWrite, "workspace.write_file missing from MCP audit log");

        // 3. Read-only mode denies workspace.write_file / exec over HTTP while permitting read_file
        Expect(ws.Configure(true, rootDir, true, cfgErr), "Reconfigure read-only failed");
        const Json readRo = callHttpTool(3, "workspace.read_file", {{"path", "hello.txt"}});
        const auto readJson = Json::parse(readRo["result"]["content"][0]["text"].get<std::string>());
        Expect(readJson["ok"] == true && readJson["content"] == "SECRET_BODY_XYZ",
               "Read-only blocked HTTP workspace.read_file");
        const Json writeRo = callHttpTool(4, "workspace.write_file", {{"path", "ro2.txt"}, {"content", "x"}});
        const auto writeRoJson = Json::parse(writeRo["result"]["content"][0]["text"].get<std::string>());
        Expect(writeRoJson["ok"] == false && writeRoJson["code"] == "LOCAL_WORKSPACE_READ_ONLY",
               "Read-only permitted HTTP workspace.write_file");

        server.Stop();
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    std::cout << "PASS: workspace.* loopback-only boundary, ephemeral HTTP port, audit sanitization and readOnly gating\n";
}

void TestProtocolVersionAndHeaderTolerance() {
    std::cout << "Running TestProtocolVersionAndHeaderTolerance...\n";
    auto dispatcher = std::make_shared<McpDispatcher>();

    McpHttpServer server;
    std::wstring err;
    Expect(server.Start(
               "127.0.0.1", 0,
               [dispatcher](const std::string& ip, const std::string& body) {
                   return dispatcher->Dispatch(ip, body);
               },
               [] { return std::string("{\"status\":\"ok\"}"); },
               err, true),
           "McpHttpServer failed to start for protocol header test");
    const uint16_t port = server.Port();

    auto sendRawPost = [port](const std::string& extraHeaders) -> std::string {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Expect(s != INVALID_SOCKET, "socket failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        Expect(connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "connect failed");
        const std::string body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";
        const std::string req =
            "POST /mcp HTTP/1.1\r\n"
            "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n" +
            extraHeaders + "\r\n" + body;
        send(s, req.data(), static_cast<int>(req.size()), 0);
        std::string resp;
        char buf[4096];
        int n = 0;
        while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
            resp.append(buf, n);
        closesocket(s);
        return resp;
    };

    for (const char* ver : {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}) {
        const std::string resp = sendRawPost(std::string("MCP-Protocol-Version: ") + ver + "\r\n");
        Expect(resp.find("HTTP/1.1 200 OK") == 0, "Supported MCP-Protocol-Version must return 200 OK");
    }
    {
        const std::string resp = sendRawPost("MCP-Protocol-Version: 1999-01-01\r\n");
        Expect(resp.find("HTTP/1.1 400 Bad Request") == 0, "Unsupported MCP-Protocol-Version must return 400");
    }
    {
        const std::string resp = sendRawPost("Accept: application/json\r\nAccept: text/event-stream\r\n");
        Expect(resp.find("HTTP/1.1 200 OK") == 0, "Benign repeated Accept headers must not fail with 400");
    }
    {
        const std::string resp = sendRawPost("Origin: http://127.0.0.1:1\r\nOrigin: http://127.0.0.1:2\r\n");
        Expect(resp.find("HTTP/1.1 400 Bad Request") == 0, "Duplicate Origin header must still return 400");
    }
    {
        const std::string resp = sendRawPost("MCP-Protocol-Version: 2025-03-26\r\nMCP-Protocol-Version: 2025-06-18\r\n");
        Expect(resp.find("HTTP/1.1 400 Bad Request") == 0, "Duplicate MCP-Protocol-Version must return 400");
    }

    server.Stop();
    std::cout << "PASS: MCP-Protocol-Version negotiation and header hardening\n";
}

} // namespace

int main() {
    WSADATA wsaData{};
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    std::cout << "========================================" << std::endl;
    std::cout << "Starting MCP Server & Protocol Integration Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    TestProtocolAndDiscovery();
    TestPowerWhitelistAndRejections();
    TestBusinessFlowAndPasswordMasking();
    TestResidentLifecycle();
    TestStatelessHttpResponseIntegrity();
    TestIpcAccessAndCancellation();
    TestWorkspaceLoopbackBoundary();
    TestProtocolVersionAndHeaderTolerance();

    std::cout << "========================================" << std::endl;
    std::cout << "All 7 MCP Server Test Suites PASSED 100%!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
