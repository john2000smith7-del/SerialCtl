// Offline process fixture only. Never shipped and never contacts an SSH host.
#include "AiProtocol.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <windows.h>
using serialctl::ai::Json;
void Send(Json value) {
    std::cout << value.dump() << '\n' << std::flush;
}
int wmain(int argc, wchar_t **argv) {
    bool key = false, probe = false, agyAcp = false, hasProcessPerSessionProbe = false;
    std::wstring passwordPath;
    std::string reverseSpec;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"-pw")
            return 40; // A plaintext command-line secret is a test failure.
        if (arg == L"-pwfile" && i + 1 < argc)
            passwordPath = argv[++i];
        if (arg == L"-R" && i + 1 < argc) {
            const std::wstring wspec(argv[++i]);
            reverseSpec.assign(wspec.begin(), wspec.end());
        }
        if (arg == L"-hostkey")
            key = true;
        if (arg == L"exit")
            probe = true;
        if (arg.find(L"agy acp") != std::wstring::npos)
            agyAcp = true;
        if (arg.find(L"--process-per-session") != std::wstring::npos)
            hasProcessPerSessionProbe = true;
    }
    if (passwordPath.rfind(L"\\\\.\\pipe\\SerialCtl-Password-", 0) != 0)
        return 41;
    auto pipe = CreateFileW(passwordPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return 42;
    std::string password;
    char c = 0;
    DWORD got = 0;
    while (ReadFile(pipe, &c, 1, &got, nullptr) && got && c != '\n')
        password += c;
    CloseHandle(pipe);
    if (password != u8"fixture-only-π")
        return 43;
    SecureZeroMemory(password.data(), password.size());
    if (probe) {
        if (key)
            return 0;
        std::cerr << "The host key is not cached for this server\nkey fingerprint is:\nssh-ed25519 255 "
                     "SHA256:44uodpMmNt68MktdLXKNyvUzZhOewpe0uE2NpYtDoHM\n";
        return 1;
    }
    if (agyAcp) {
        if (reverseSpec.empty())
            return 45;
        Send({{"jsonrpc", "2.0"},
              {"method", "fixture/reverseTunnel"},
              {"params", {{"spec", reverseSpec}, {"hasProcessPerSessionProbe", hasProcessPerSessionProbe}}}});
        const char *fwdFailed = std::getenv("SERIALCTL_FIXTURE_FWD_FAILED");
        if (fwdFailed && fwdFailed[0] == '1')
            std::cerr << "Warning: remote port forwarding failed for listen port 17081\n" << std::flush;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto message = Json::parse(line);
        if (!message.contains("method"))
            continue;
        const auto method = message.at("method").get<std::string>();
        const auto id = message.value("id", Json());
        if (method == "initialize") {
            Json initResult = {{"userAgent", "offline-fixture"}};
            const char *httpFalse = std::getenv("SERIALCTL_FIXTURE_MCP_HTTP_FALSE");
            if (httpFalse && httpFalse[0] == '1')
                initResult["agentCapabilities"] = {{"mcpCapabilities", {{"http", false}}}};
            const char *acpLoad = std::getenv("SERIALCTL_FIXTURE_ACP_LOAD");
            if (acpLoad && (*acpLoad == '1' || std::string(acpLoad) == "fallback")) {
                initResult["protocolVersion"] = 1;
                if (!initResult.contains("agentCapabilities"))
                    initResult["agentCapabilities"] = Json::object();
                initResult["agentCapabilities"]["loadSession"] = true;
                initResult["agentCapabilities"]["sessionCapabilities"] = {{"resume", Json::object()}};
            }
            Send({{"id", id}, {"result", initResult}});
        } else if (method == "model/list") {
            Send({{"method", "remoteControl/status/changed"}, {"params", {{"status", "disabled"}}}});
            Send({{"id", id},
                  {"result",
                   {{"data",
                     Json::array({{{"id", "fixture"},
                                   {"model", "fixture"},
                                   {"displayName", "Offline fixture"},
                                   {"isDefault", true},
                                   {"supportedReasoningEfforts", Json::array({{{"reasoningEffort", "high"}}})}}})},
                    {"nextCursor", nullptr}}}});
        } else if (method == "config/read")
            Send({{"id", id}, {"result", {{"config", {{"model", "fixture"}}}}}});
        else if (method == "command/exec") {
            Send({{"id",id},{"result",{{"exitCode",0},{"stdout","/home/linux\n"},{"stderr",""}}}});
        } else if (method == "fs/readDirectory") {
            Send({{"id",id},{"result",{{"entries",Json::array({{{"fileName","fixture-project"},{"isDirectory",true},{"isFile",false}}})}}}});
        } else if (method == "thread/start") {
            const auto toolCount = message["params"].value("dynamicTools", Json::array()).size();
            if (toolCount != 15 && toolCount != serialctl::ai::AllDynamicTools().size())
                return 44;
            if (message["params"].contains("config") &&
                message["params"]["config"].value("web_search", Json()) != "disabled") {
                Send(
                    {{"id", id},
                     {"error",
                      {{"code", -32600}, {"message", "failed to load configuration: expected string in web_search"}}}});
                continue;
            }
            Send({{"id", id}, {"result", {{"thread", {{"id", "fixture-thread"}}}}}});
        } else if (method == "turn/start") {
            Send({{"id", id}, {"result", {{"turn", {{"id", "fixture-turn"}}}}}});
            const auto event = Json{{"method", "item/agentMessage/delta"},
                                    {"params",
                                     {{"threadId", "fixture-thread"},
                                      {"turnId", "fixture-turn"},
                                      {"itemId", "reply"},
                                      {"delta", u8"读取真实串口日志。"}}}}
                                   .dump() +
                               "\n";
            for (char byte : event) {
                std::cout.put(byte);
                std::cout.flush();
            }
            Send({{"method", "item/completed"},
                  {"params",
                   {{"threadId", "fixture-thread"},
                    {"turnId", "fixture-turn"},
                    {"item",
                     {{"type", "agentMessage"},
                      {"id", "reply"},
                      {"text", u8"读取真实串口日志。"},
                      {"phase", nullptr}}}}}});
            Send({{"id", 501},
                  {"method", "item/tool/call"},
                  {"params",
                   {{"threadId", "fixture-thread"},
                    {"turnId", "fixture-turn"},
                    {"callId", "read-1"},
                    {"tool", "serialctl_read"},
                    {"arguments", {{"sessionId", "session-7"}, {"lines", 200}}}}}});
        } else if (method == "turn/interrupt") {
            Send({{"id", id}, {"result", Json::object()}});
            Send({{"method", "turn/completed"},
                  {"params",
                   {{"threadId", "fixture-thread"}, {"turn", {{"id", "fixture-turn"}, {"status", "interrupted"}}}}}});
        } else if (method == "session/load") {
            const char *acpLoad = std::getenv("SERIALCTL_FIXTURE_ACP_LOAD");
            if (acpLoad && std::string(acpLoad) == "fallback") {
                Send({{"id", id}, {"error", {{"code", -32601}, {"message", "Method not found"}}}});
                continue;
            }
            const auto sid = message["params"].value("sessionId", std::string("acp-hist-7"));
            auto sendUpdate = [&](Json update) {
                Send({{"jsonrpc", "2.0"},
                      {"method", "session/update"},
                      {"params", {{"sessionId", sid}, {"update", std::move(update)}}}});
            };
            sendUpdate({{"sessionUpdate", "user_message_chunk"},
                        {"content", {{"type", "text"}, {"text", u8"分析工程\n\n[SerialCtl associated terminal session: id=session-7]"}}}});
            sendUpdate({{"sessionUpdate", "agent_thought_chunk"},
                        {"content", {{"type", "text"}, {"text", u8"### 步骤\n- 查看目录"}}}});
            sendUpdate({{"sessionUpdate", "agent_message_chunk"},
                        {"content", {{"type", "text"}, {"text", u8"先列出工作空间目录。"}}}});
            sendUpdate({{"sessionUpdate", "tool_call"},
                        {"toolCallId", "replay-tool-1"},
                        {"title", "Run mcp_serialctl_workspace.list"},
                        {"status", "in_progress"},
                        {"rawInput", {{"depth", 2}}}});
            sendUpdate({{"sessionUpdate", "tool_call_update"},
                        {"toolCallId", "replay-tool-1"},
                        {"status", "completed"},
                        {"rawOutput", {{"entries", Json::array({"src/", "CMakeLists.txt"})}}}});
            sendUpdate({{"sessionUpdate", "agent_message_chunk"},
                        {"content", {{"type", "text"}, {"text", u8"已完成目录检查。"}}}});
            Send({{"id", id}, {"result", {{"modes", {{"currentModeId", "code"}}}}}});
        } else if (method == "session/resume") {
            Send({{"id", id}, {"result", {{"modes", {{"currentModeId", "code"}}}}}});
        }
    }
}
