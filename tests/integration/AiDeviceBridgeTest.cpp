#include "AiDeviceBridge.h"
#include <iostream>
#include <stdexcept>
using namespace serialctl;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class Transport final : public IConnection {
public:
    unsigned writes = 0; Bytes last; std::vector<Bytes> history; bool accept = true;
    bool Start(DataCallback, StatusCallback, std::wstring&) override { return true; }
    void Stop() override {}
    bool IsConnected() const override { return true; }
    bool Send(const Bytes& data, std::wstring&) override { ++writes; last = data; history.push_back(data); return accept; }
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
        const auto initialPower = power.State();
        Check(!PowerService::ConnectionAvailable() && !request("power.get")["connected"].get<bool>(),
              "production AI bridge must report disconnected power");
        for (const bool enabled : {false, true}) {
            const auto result = request("power.output", {{"channels", {1}}, {"enabled", enabled},
                                                         {"requestId", "disabled-power"}});
            Check(result.contains("error") && !result.contains("id") && power.State() == initialPower,
                  "AI cannot queue an action or change disabled power state");
        }
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
        auto enter=request("boot_enter",{{"preset","uboot_space"}});
        Check(!enter.contains("error") && enter.value("intervalMs",0)==300 && enter.value("untilStopped",false) && pulse && transport->last==Bytes{' '},"Restart is followed immediately by a local space");
        Check(transport->history[transport->history.size()-2]==Bytes({'r','e','b','o','o','t','\r'}),"Reboot and interrupt ordering changed");
        const auto rebootWrites=transport->writes;
        Check(request("command",{{"command","echo unrelated"}}).contains("error") && request("boot_arm").contains("error") && transport->writes==rebootWrites,"Other AI commands or legacy capture interleaved a continuous boot task");
        Check(request("boot_enter",{{"preset","uboot_space"}}).contains("error") && transport->writes==rebootWrites,"Repeated tool call rebooted the board twice");
        now+=300;AiDeviceBridge::Pulse(state,transport,now);
        Check(transport->writes==rebootWrites+1,"Native timer does not continue interrupting without model calls");
        request("release");
        targets[0].mode=0;
        Check(request("boot_enter",{{"preset","phytium_uefi_f2"}}).contains("error"),"Boot preset rebooted an SSH server");
        targets[0].mode=1;
        Check(!request("boot_enter",{{"preset","phytium_uefi_f2"},{"intervalMs",500}}).contains("error") && transport->last==Bytes({27,'O','Q'}),"Phytium preset did not send a real F2 key");
        Check(request("boot_status").value("intervalMs",0)==500,"Selected cadence was not exposed in task status");
        request("boot_stop");const auto stoppedWrites=transport->writes;now+=300;AiDeviceBridge::Pulse(state,transport,now);
        Check(transport->writes==stoppedWrites,"Stopped task continues sending keys");request("release");
        transport->accept=false;const auto rejectedWrites=transport->writes;
        Check(request("boot_enter",{{"preset","uboot_space"}}).contains("error") && transport->writes==rejectedWrites+1 && !pulse,"Rejected restart emitted interrupt keys");
        power.Shutdown();
        std::cout << "AI bridge binding, observation bounds, boot release and recovery evidence passed; no physical devices\n";
    } catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
    return 0;
}
