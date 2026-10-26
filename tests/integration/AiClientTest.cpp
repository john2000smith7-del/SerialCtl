#include "CodexClient.h"
#include "CodexSession.h"
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <windows.h>

using namespace serialctl::ai;
void Require(bool value, const char *reason) {
    if (!value)
        throw std::runtime_error(reason);
}
int main() {
    CodexClient client;
    CodexSession session;
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Json> events;
    ServerSettings settings;
    settings.host = L"serialctl-offline-" + std::to_wstring(GetCurrentProcessId()) + L".invalid";
    settings.username = L"fixture";
    settings.password = L"fixture-only-π";
    int confirmations = 0;
    auto wait = [&](const std::function<bool(const Json &)> &predicate) {
        std::unique_lock<std::mutex> lock(mutex);
        const bool found = wake.wait_for(lock, std::chrono::seconds(10), [&] {
            for (const auto &e : events)
                if (predicate(e))
                    return true;
            return false;
        });
        Require(found, "Timed out waiting for fixture JSON-RPC event");
    };
    try {
        std::wstring validation;
        Require(IsAuthenticationFailure(L"Access denied") &&
                    IsAuthenticationFailure(L"No supported authentication methods available"),
                "Password failure not classified");
        Require(!IsAuthenticationFailure(L"Host key did not match manually configured list") &&
                    !IsAuthenticationFailure(L"Connection timed out"),
                "Host identity or network error misclassified as password failure");
        Require(ForgetSavedServerPassword(settings, validation),
                "Removing absent fixture password should be idempotent");
        settings.workingDirectory = L"relative/path";
        Require(!ValidateServer(settings, validation), "Relative server project directory accepted");
        settings.workingDirectory = L"/srv/code\nother";
        Require(!ValidateServer(settings, validation), "Multiline project directory accepted");
        settings.workingDirectory = L"/srv/fixture code";
        Require(ValidateServer(settings, validation), "Absolute directory with spaces rejected");
        Require(client.Connect(
                    settings,
                    [&](Json e) {
                        std::lock_guard<std::mutex> l(mutex);
                        events.push_back(std::move(e));
                        wake.notify_all();
                    },
                    [&](const serialctl::HostKeyChallenge& challenge) {
                        ++confirmations;
                        return challenge.fingerprint.find(L"SHA256:44uodpMmNt68MktdLXKNyvUzZhOewpe0uE2NpYtDoHM") != std::wstring::npos;
                    }),
                "Connect rejected");
        wait([](const Json &e) { return e.value("event", std::string()) == "ready"; });
        Require(client.Ready() && confirmations == 1,
                "Host identity must be explicitly confirmed once before readiness");
        Require(client.Send({{"id", 11}, {"method", "model/list"}, {"params", {{"limit", 100}}}}),
                "Model request rejected");
        wait([](const Json &e) {
            return e.value("id", Json()) == 11 && e.contains("result") && e["result"]["data"].size() == 1;
        });
        Require(client.Send({{"id", 12}, {"method", "thread/start"}, {"params", {{"dynamicTools", SerialTools()}}}}),
                "Thread request rejected");
        wait([](const Json &e) { return e.value("id", Json()) == 12; });
        Require(client.Send({{"id", 13},
                             {"method", "turn/start"},
                             {"params", {{"threadId", "fixture-thread"}, {"input", Json::array()}}}}),
                "Turn request rejected");
        wait([](const Json &e) {
            return e.value("method", std::string()) == "item/agentMessage/delta" &&
                   e["params"]["delta"] == u8"读取真实串口日志。";
        });
        wait([](const Json &e) {
            return e.value("id", Json()) == 501 && e.value("method", std::string()) == "item/tool/call";
        });
        Require(client.Send({{"id", 501}, {"result", ToolResult({{"error", "User declined"}}, false)}}),
                "Tool response rejected");
        Require(client.Send({{"id", 14},
                             {"method", "turn/interrupt"},
                             {"params", {{"threadId", "fixture-thread"}, {"turnId", "fixture-turn"}}}}),
                "Interrupt request rejected");
        wait([](const Json &e) { return e.value("method", std::string()) == "turn/completed"; });
        client.Shutdown();
        Require(!client.Ready() && !client.Running(), "Worker did not shut down");
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.clear();
        }
        Require(session.Connect(
                    settings,
                    [&](Json e) {
                        std::lock_guard<std::mutex> lock(mutex);
                        events.push_back(std::move(e));
                        wake.notify_all();
                    },
                    [](const serialctl::HostKeyChallenge&) { return false; }),
                "Domain session connect rejected");
        wait([](const Json &e) { return e.value("event", std::string()) == "ready"; });
        Require(session.Request(Operation::Models, 21, Json::object()), "Domain model request rejected");
        wait([](const Json &e) {
            return e.value("token", Json()) == 21 && e.contains("result") &&
                   e["result"]["models"][0]["id"] == "fixture" && e["result"]["models"][0]["reasoning"] == true;
        });
        Require(session.Request(Operation::NewConversation, 22, {{"defaultModel", "fixture"}, {"cwd", "/srv/fixture code"}}),
                "Domain conversation request rejected");
        wait([](const Json &e) {
            return e.value("token", Json()) == 22 && e.contains("result") &&
                   e["result"]["conversationId"] == "fixture-thread";
        });
        Require(
            session.Request(Operation::StartTask, 23, {{"conversationId", "fixture-thread"}, {"text", u8"读取日志"}}),
            "Domain task request rejected");
        wait([](const Json &e) {
            return e.value("event", std::string()) == "textDelta" && e["text"] == u8"读取真实串口日志。";
        });
        wait([](const Json &e) {
            return e.value("event", std::string()) == "textComplete" && e["final"] == true &&
                   e["text"] == u8"读取真实串口日志。";
        });
        wait([](const Json &e) {
            return e.value("event", std::string()) == "serialToolRequest" && e["requestId"] == 501 &&
                   e["tool"] == "serialctl_read" && e["arguments"]["sessionId"] == "session-7";
        });
        Require(session.ReplyTool(501, {{"text", "fixture log"}}, true), "Domain tool reply rejected");
        Require(session.Request(Operation::StopTask, 24,
                                {{"conversationId", "fixture-thread"}, {"taskId", "fixture-turn"}}),
                "Domain stop request rejected");
        wait([](const Json &e) {
            return e.value("event", std::string()) == "taskFinished" && e["status"] == "interrupted";
        });
        session.Shutdown();
        Require(!session.Ready() && !session.Running(), "Domain session did not shut down");
        std::cout
            << "AiClient: secure password pipe, explicit host verification, handshake, model discovery, UTF-8 "
               "streaming, dynamic tool request, cancellation and UI domain adapter passed (offline process fixture)\n";
    } catch (const std::exception &e) {
        session.Shutdown();
        client.Shutdown();
        std::cerr << e.what() << '\n';
        return 1;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SerialCtl\\TrustedHostKeys", 0, KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(key, (settings.host + L":22").c_str());
        RegCloseKey(key);
    }
}
