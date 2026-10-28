#include "AiDeviceBridge.h"
#include <iostream>
#include <stdexcept>
using namespace serialctl;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class Transport final : public IConnection {
public:
    unsigned writes = 0; Bytes last;
    bool Start(DataCallback, StatusCallback, std::wstring&) override { return true; }
    void Stop() override {}
    bool IsConnected() const override { return true; }
    bool Send(const Bytes& data, std::wstring&) override { ++writes; last = data; return true; }
};
}
int main() {
    try {
        PowerService power;
        SessionService sessions;
        AiDeviceBridge bridge(power, sessions);
        AiDeviceBridge::State state;
        TerminalModel model;
        auto transport = std::make_shared<Transport>();
        const std::wstring name = L"controlled board", empty;
        std::vector<AiDeviceBridge::Target> targets{{state, model, 42, 1, name, transport, CP_UTF8,
                                                   empty, empty, empty, 0, false, false, 0}};
        sessions.Register({{"id", "session-42"}, {"kind", "serial"}}, transport);
        AiDeviceBridge::Context context{"instance", false, Json::array({1}),
            {{"enabled", true}, {"sessionId", "session-42"}, {"channels", {1}}, {"identity", "test"}, {"mode", "independent"}}};
        ULONGLONG now = 10000; bool pulse = false;
        auto request = [&](const std::string& method, Json params = Json::object()) {
            params["sessionId"] = "session-42";
            return bridge.Request(method, params, targets, context, now, pulse);
        };
        auto sent = request("command", {{"command", "echo test"}});
        Check(!sent.contains("error") && sent["commandId"] == 1 && transport->last.back() == '\r', "AI submits only to explicit existing session");
        Check(request("recover").contains("error"), "fresh command is not hang evidence");
        now += 6000;
        Check(request("recover").contains("error"), "unobserved command cannot authorize recovery");
        request("read");
        state.aiLastReceivedAt = now;
        Check(request("recover").contains("error"), "recent device output prevents inactivity recovery");
        state.aiLastReceivedAt = 0;
        state.aiCursor = state.aiCommandCursor + 1;
        model.Feed(L"board#", L"");
        Check(request("recover").contains("error"), "visible prompt is not a hang");
        model.Clear(); state.aiCommandText = "flash erase";
        Check(request("recover").contains("error"), "non-interruptible operation cannot authorize automatic power recovery");
        context.recoveryBinding["sessionId"] = "session-43";
        Check(request("recover").contains("error"), "recovery remains bound to exact board identity");
        context.macroRunning = true;
        const auto writes = transport->writes;
        Check(request("command", {{"command", "no"}}).contains("error") &&
              request("key", {{"key", "Enter"}}).contains("error") && transport->writes == writes,
              "macro gate prevents interleaved AI writes");
        context.macroRunning = false;
        const auto armed = request("boot_arm");
        Check(armed.value("state", "") == "armed" && armed.value("error", "unexpected").empty() &&
              pulse && state.boot->Status(now).state == "armed", "boot policy requests UI scheduling without retaining GUI");
        request("release");
        Check(state.boot->Status(now).state == "canceled" && !pulse, "turn release cancels capture and clears scheduling output");
        for (unsigned i = 0; i < 300; ++i)
            AiDeviceBridge::Observe(state, std::wstring(500, L'中'), L"controlled timestamp", now);
        Check(state.aiOutput.size() <= 256 && state.aiBytes <= 65536, "AI observation memory and entry count remain bounded");
        const auto read = request("read", {{"after", 1}});
        Check(read["gap"] == true && read["truncated"] == true && read["bytes"].get<size_t>() <= 65536,
              "evicted evidence explicitly reports gap and truncation");
        auto unknown = bridge.Request("command", {{"sessionId", "session-999"}, {"command", "no"}}, targets, context, now, pulse);
        Check(unknown.contains("error") && transport->writes == writes, "unassociated session never auto-opens or receives bytes");
        power.Shutdown();
        std::cout << "AI bridge binding, observation bounds, boot release and recovery evidence passed; no physical devices\n";
    } catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
    return 0;
}
