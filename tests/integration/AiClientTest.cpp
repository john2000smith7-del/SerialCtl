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
        Require(settings.provider == AiProvider::Codex && !settings.localMode,
                "ServerSettings must default to Codex and remote SSH mode");
        {
            ServerSettings localSettings;
            localSettings.localMode = true;
            localSettings.provider = AiProvider::Antigravity;
            localSettings.workingDirectory = L"D:\\workspace\\proj";
            Require(ValidateServer(localSettings, validation),
                    "Local mode must allow empty SSH host/user/password");
            localSettings.workingDirectory = L"D:\\bad\npath";
            Require(!ValidateServer(localSettings, validation),
                    "Local mode must still reject multiline working directory");
        }
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

        // Verify Antigravity SSH reverse MCP tunnel (-R) without exposing plaintext passwords
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.clear();
        }
        ServerSettings agySettings = settings;
        agySettings.provider = AiProvider::Antigravity;
        agySettings.mcpPort = 7081;
        agySettings.remoteMcpPort = 17081;
        Require(client.Connect(
                    agySettings,
                    [&](Json e) {
                        std::lock_guard<std::mutex> lock(mutex);
                        events.push_back(std::move(e));
                        wake.notify_all();
                    },
                    [](const serialctl::HostKeyChallenge&) { return false; }),
                "Antigravity fixture connect rejected");
        wait([](const Json &e) { return e.value("event", std::string()) == "ready"; });
        wait([](const Json &e) {
            return e.value("method", std::string()) == "fixture/reverseTunnel" &&
                   e.contains("params") &&
                   e["params"].value("spec", std::string()) == "127.0.0.1:17081:127.0.0.1:7081" &&
                   e["params"].value("hasProcessPerSessionProbe", false) == true;
        });
        client.Shutdown();

        {
            struct EnvGuard {
                ~EnvGuard() {
                    SetEnvironmentVariableW(L"SERIALCTL_FIXTURE_MCP_HTTP_FALSE", nullptr);
                    SetEnvironmentVariableW(L"SERIALCTL_FIXTURE_FWD_FAILED", nullptr);
                }
            } envGuard;
            SetEnvironmentVariableW(L"SERIALCTL_FIXTURE_MCP_HTTP_FALSE", L"1");
            SetEnvironmentVariableW(L"SERIALCTL_FIXTURE_FWD_FAILED", L"1");
            {
                std::lock_guard<std::mutex> lock(mutex);
                events.clear();
            }
            ServerSettings diagSettings = settings;
            diagSettings.provider = AiProvider::Antigravity;
            diagSettings.localMode = false;
            diagSettings.mcpPort = 7081;
            diagSettings.remoteMcpPort = 17081;
            Require(client.Connect(
                        diagSettings,
                        [&](Json e) {
                            std::lock_guard<std::mutex> lock(mutex);
                            events.push_back(std::move(e));
                            wake.notify_all();
                        },
                        [](const serialctl::HostKeyChallenge&) { return false; }),
                    "Antigravity diagnostics fixture connect rejected");
            wait([](const Json &e) {
                return e.value("event", std::string()) == "status" &&
                       e.value("message", std::string()).find("AllowTcpForwarding") != std::string::npos;
            });
            wait([](const Json &e) {
                return e.value("event", std::string()) == "status" &&
                       e.value("message", std::string()).find("mcpCapabilities.http=false") != std::string::npos;
            });
            wait([](const Json &e) { return e.value("event", std::string()) == "ready"; });
            client.Shutdown();
        }

        // Verify Antigravity ACP session/load replay and -32601 fallback to session/resume
        {
            struct AcpEnvGuard {
                CodexSession &session;
                ~AcpEnvGuard() {
                    session.Shutdown();
                    SetEnvironmentVariableW(L"SERIALCTL_FIXTURE_ACP_LOAD", nullptr);
                }
            } acpGuard{session};

            SetEnvironmentVariableW(L"SERIALCTL_FIXTURE_ACP_LOAD", L"1");
            {
                std::lock_guard<std::mutex> lock(mutex);
                events.clear();
            }
            ServerSettings acpSettings = settings;
            acpSettings.provider = AiProvider::Antigravity;
            acpSettings.mcpPort = 7081;
            acpSettings.remoteMcpPort = 17081;
            Require(session.Connect(
                        acpSettings,
                        [&](Json e) {
                            std::lock_guard<std::mutex> lock(mutex);
                            events.push_back(std::move(e));
                            wake.notify_all();
                        },
                        [](const serialctl::HostKeyChallenge &) { return false; }),
                    "Antigravity ACP loadSession fixture connect rejected");
            wait([](const Json &e) {
                return e.value("event", std::string()) == "ready" && e.value("acpLoadSession", false);
            });
            Require(session.Request(Operation::RestoreConversation, 31,
                                    {{"conversationId", "acp-hist-7"}, {"cwd", "/srv/proj"}}),
                    "Antigravity session/load RestoreConversation rejected");
            wait([](const Json &e) {
                return e.value("token", Json()) == 31 && e.contains("result") &&
                       e["result"].value("conversationId", std::string()) == "acp-hist-7" &&
                       e["result"].value("messages", Json::array()).size() == 5;
            });
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = std::find_if(events.begin(), events.end(), [](const Json &e) {
                    return e.value("token", Json()) == 31 && e.contains("result");
                });
                Require(it != events.end(), "Missing ACP RestoreConversation response");
                const auto &msgs = (*it)["result"]["messages"];
                Require(msgs[0]["role"] == u8"我" && msgs[0]["text"] == u8"分析工程" &&
                            msgs[1]["role"] == u8"思考" &&
                            msgs[2]["role"] == "AI" && msgs[2]["process"] == true &&
                            msgs[3]["role"] == u8"工具" &&
                            msgs[3]["text"].get<std::string>().find("CMakeLists.txt") != std::string::npos &&
                            msgs[3].contains("technical") &&
                            msgs[4]["role"] == "AI" && msgs[4]["process"] == false,
                        "ACP session/load replay must decode user, thought, commentary, tool output and final answer");
            }
            session.Shutdown();

            SetEnvironmentVariableW(L"SERIALCTL_FIXTURE_ACP_LOAD", L"fallback");
            {
                std::lock_guard<std::mutex> lock(mutex);
                events.clear();
            }
            Require(session.Connect(
                        acpSettings,
                        [&](Json e) {
                            std::lock_guard<std::mutex> lock(mutex);
                            events.push_back(std::move(e));
                            wake.notify_all();
                        },
                        [](const serialctl::HostKeyChallenge &) { return false; }),
                    "Antigravity ACP fallback fixture connect rejected");
            wait([](const Json &e) { return e.value("event", std::string()) == "ready"; });
            Require(session.Request(Operation::RestoreConversation, 32,
                                    {{"conversationId", "acp-hist-8"}, {"cwd", "/srv/proj"}}),
                    "Antigravity fallback RestoreConversation rejected");
            wait([](const Json &e) {
                return e.value("token", Json()) == 32 && e.contains("result") &&
                       e["result"].value("conversationId", std::string()) == "acp-hist-8";
            });
            session.Shutdown();
        }

        std::cout
            << "AiClient: secure password pipe, explicit host verification, handshake, model discovery, UTF-8 "
               "streaming, dynamic tool request, cancellation, SSH -R MCP tunnel and UI domain adapter passed (offline process fixture)\n";
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
