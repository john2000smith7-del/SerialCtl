#undef NDEBUG
#include <cassert>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "McpProtocol.h"
#include "McpSecurity.h"
#include "McpDispatcher.h"
#include "McpHttpServer.h"
#include "McpService.h"
#include "PowerService.h"

using namespace serialctl;
using namespace serialctl::mcp;

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Assertion failed: " << message << std::endl;
        assert(false);
    }
}

std::string HttpPost(uint16_t port, const std::string& path, const std::string& body) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s != INVALID_SOCKET);

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
                      "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

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

std::string HttpGet(uint16_t port, const std::string& path) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s != INVALID_SOCKET);

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
    Expect(initResp["result"]["protocolVersion"] == "2024-11-05", "protocolVersion must be 2024-11-05");
    Expect(initResp["result"]["serverInfo"]["name"] == "serialctl-mcp", "Server name must match");
    Expect(initResp["result"]["capabilities"].contains("tools"), "Must declare tools capability");

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

    // Verify key categories exist
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
    Expect(hasTool("xfer.start"), "Must contain xfer.start");
    Expect(hasTool("tftp.publish_local"), "Must contain tftp.publish_local");
    Expect(hasTool("power.get"), "Must contain power.get");
    Expect(hasTool("power.output"), "Must contain power.output");
    Expect(hasTool("power.action"), "Must contain power.action");
    Expect(hasTool("power.recover"), "Must contain power.recover");
    Expect(hasTool("log.export"), "Must contain log.export");
    Expect(hasTool("ui.theme"), "Must contain ui.theme");
    Expect(hasTool("ai.send"), "Must contain ai.send");

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
    power.AuthorizeTest(L"test");

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
    dispatcher.SetGuiExecutor([&power](const std::string& method, const Json& params) -> Json {
        if (method == "power.get") {
            return power.State();
        }
        if (method == "power.output") {
            return power.Submit({
                {"type", "output"},
                {"channels", params.value("channels", Json::array({1}))},
                {"enabled", params.value("enabled", false)}
            });
        }
        if (method == "power.action") {
            return power.Action(params.at("id").get<std::string>());
        }
        return {{"error", "Unknown"}};
    });

    // 1. Positive: power.get
    Json getResult = dispatcher.CallTool("127.0.0.1", "power.get", Json::object());
    Expect(getResult.contains("content"), "power.get must return content");
    Expect(!getResult.value("isError", false), "power.get must not be error");

    // 2. Positive: power.output enabled=true on checked channel 1
    Json outResult = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"reason", "Test energize"}
    });
    Expect(outResult.contains("content"), "power.output must return content");
    Expect(!outResult.value("isError", false), "power.output on selected channels must succeed");

    // 3. Negative: Attempt to set voltage -> Hard rejection
    Json voltCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"voltage", 5.0}
    });
    Expect(voltCall.contains("error"), "voltage modification must be rejected");
    Expect(voltCall["error"]["code"] == ErrPowerParameterForbidden, "Must return ErrPowerParameterForbidden");
    Expect(voltCall["error"]["message"].get<std::string>().find("POWER_PARAMETER_CHANGE_FORBIDDEN") != std::string::npos,
           "Must contain POWER_PARAMETER_CHANGE_FORBIDDEN in message");

    // 4. Negative: Attempt to set current -> Hard rejection
    Json currCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"current", 2.0}
    });
    Expect(currCall["error"]["code"] == ErrPowerParameterForbidden, "current modification must be rejected");

    // 5. Negative: Attempt to set OVP / OCP -> Hard rejection
    Json ovpCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"ovp", 6.5}
    });
    Expect(ovpCall["error"]["code"] == ErrPowerParameterForbidden, "ovp modification must be rejected");

    // 6. Negative: Attempt to set mode (series/parallel) -> Hard rejection
    Json modeCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"mode", "parallel"}
    });
    Expect(modeCall["error"]["code"] == ErrPowerParameterForbidden, "mode modification must be rejected");

    // 7. Negative: Attempt to override channels (tamper GUI selection) -> Hard rejection
    Json chanCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"channels", Json::array({1, 2, 3})} // GUI has only [1]
    });
    Expect(chanCall["error"]["code"] == ErrPowerParameterForbidden, "channel tampering must be rejected");

    // 8. Negative: Attempt to send raw SCPI -> Hard rejection
    Json scpiCall = dispatcher.CallTool("127.0.0.1", "power.output", {
        {"enabled", true},
        {"scpi", "*IDN?\nVOLT 10"}
    });
    Expect(scpiCall["error"]["code"] == ErrPowerParameterForbidden, "SCPI modification must be rejected");

    // 9. Negative: Calling unexposed tools -> Hard rejection
    for (const char* unexposed : {"power.set", "power.calibrate", "power.reset", "power.preset", "power.voltage"}) {
        Json unexpCall = dispatcher.CallTool("127.0.0.1", unexposed, Json::object());
        Expect(unexpCall.contains("error") && unexpCall["error"]["code"] == ErrPowerParameterForbidden,
               "Unexposed power tools must be hard rejected");
    }

    // 10. Verify zero side-effects on power supply state
    auto finalState = power.State();
    Expect(finalState.value("connected", false), "Power supply remains connected");
    // Verify default voltage was not modified to 5.0 or 10.0
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
    dispatcher.SetGuiLauncher([&guiRunning, &launchCount](bool, std::string& err) {
        if (McpService::Instance().IsRunning() && false) {} // dummy use
        if (serialctl::mcp::McpSecurity::ValidateAiRecursion(Json::object()).errorCode != 0) {}
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

    // 5. Test service stop closes HTTP port
    McpHttpServer httpSrv;
    std::wstring err;
    const uint16_t testPort = 7182;
    httpSrv.Start("127.0.0.1", testPort,
        [&dispatcher](const std::string& ip, const std::string& body) {
            return dispatcher.Dispatch(ip, body);
        },
        [] { return "{\"status\":\"ok\"}"; },
        err);

    std::string testPing = HttpGet(testPort, "/health");
    Expect(!testPing.empty(), "Port must be open while server is running");

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

    std::cout << "========================================" << std::endl;
    std::cout << "All 4 MCP Server Test Suites PASSED 100%!" << std::endl;
    std::cout << "========================================" << std::endl;

    WSACleanup();
    return 0;
}
