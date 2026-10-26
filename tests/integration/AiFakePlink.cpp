// Offline process fixture only. Never shipped and never contacts an SSH host.
#include "AiProtocol.h"
#include <iostream>
#include <string>
#include <windows.h>
using serialctl::ai::Json;
void Send(Json value) {
    std::cout << value.dump() << '\n' << std::flush;
}
int wmain(int argc, wchar_t **argv) {
    bool key = false, probe = false;
    std::wstring passwordPath;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"-pw")
            return 40; // A plaintext command-line secret is a test failure.
        if (arg == L"-pwfile" && i + 1 < argc)
            passwordPath = argv[++i];
        if (arg == L"-hostkey")
            key = true;
        if (arg == L"exit")
            probe = true;
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
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto message = Json::parse(line);
        if (!message.contains("method"))
            continue;
        const auto method = message.at("method").get<std::string>();
        const auto id = message.value("id", Json());
        if (method == "initialize")
            Send({{"id", id}, {"result", {{"userAgent", "offline-fixture"}}}});
        else if (method == "model/list") {
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
        else if (method == "thread/start") {
            if (message["params"].value("dynamicTools", Json::array()).size() != 7)
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
        }
    }
}
