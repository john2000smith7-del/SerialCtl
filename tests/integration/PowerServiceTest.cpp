#include "PowerService.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>
namespace serialctl {
// Stop the real worker at its queue boundary, without sleeping to guess its state.
// Only the test target compiles this seam; the production GUI has no pause API.
struct PowerServiceTestAccess {
    static bool Pause(PowerService &power) {
        std::unique_lock<std::mutex> lock(power.mutex_);
        power.testPauseRequested_ = true;
        power.wake_.notify_all();
        return power.wake_.wait_for(lock, std::chrono::seconds(2), [&] { return power.testWorkerPaused_; });
    }
    static void Resume(PowerService &power) {
        std::lock_guard<std::mutex> lock(power.mutex_);
        power.testPauseRequested_ = false;
        power.wake_.notify_all();
    }
    static size_t Depth(PowerService &power) {
        std::lock_guard<std::mutex> lock(power.mutex_);
        return power.queue_.size();
    }
};
} // namespace serialctl
using namespace serialctl;
int failures = 0;
void Expect(bool ok, const char *message) {
    if (!ok) {
        std::cerr << "FAIL " << message << "\n";
        ++failures;
    }
}
Json Wait(PowerService &power, Json action) {
    if (action.contains("error"))
        return action;
    for (int i = 0; i < 100; ++i) {
        auto result = power.Action(action["id"]);
        if (result["state"] != "queued" && result["state"] != "running")
            return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {{"error", "Timeout"}};
}

void TestSafetyAtCapacity(Json safety) {
    PowerService power;
    Expect(Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"}}))["state"] == "completed",
           "capacity test connects the production simulated backend");
    Wait(power, power.Submit({{"type", "output"}, {"channels", {3}}, {"enabled", true}}));
    Wait(power, power.Submit({{"type", "task"}, {"channels", {1}},
                             {"onMs", 10000}, {"offMs", 10000}, {"count", 10}}));
    const bool paused = PowerServiceTestAccess::Pause(power);
    Expect(paused, "worker acknowledges pause before the queue is filled");
    if (!paused) return;

    std::vector<std::string> pending;
    for (int i = 0; i < 128; ++i) {
        auto action = power.Submit({{"type", "output"}, {"channels", {2}}, {"enabled", true},
                                    {"requestId", "capacity-on-" + std::to_string(i)}});
        Expect(!action.contains("error"), "all 128 pending ON commands are accepted");
        if (!action.contains("error")) pending.push_back(action["id"]);
    }
    Expect(PowerServiceTestAccess::Depth(power) == 128, "test reaches the actual queue capacity");
    auto invalid = power.Submit({{"type", "output"}, {"channels", {4}}, {"enabled", false}});
    Expect(invalid.contains("error") && PowerServiceTestAccess::Depth(power) == 128,
           "invalid safety request does not cancel queued work");
    auto conflict = power.Submit({{"type", "output"}, {"channels", {2}}, {"enabled", false},
                                  {"requestId", "capacity-on-0"}});
    Expect(conflict.contains("error") && PowerServiceTestAccess::Depth(power) == 128,
           "conflicting request ID does not cancel queued work");

    auto accepted = power.Submit(safety);
    Expect(!accepted.contains("error") && accepted.value("state", "") == "queued",
           "OFF/stop/disconnect at capacity is accepted as queued, not completed");
    Expect(PowerServiceTestAccess::Depth(power) == 1, "safety command replaces the pending ON backlog");
    for (const auto &id : pending)
        Expect(power.Action(id).value("state", "") == "canceled", "every stale pending ON is canceled");
    PowerServiceTestAccess::Resume(power);

    auto result = Wait(power, accepted);
    Expect(result.value("state", "") == "completed", "accepted safety command executes on the real worker");
    auto state = power.State();
    if (safety["type"] == "disconnect") {
        Expect(!state.value("connected", true) && state["channels"][0]["output"].is_null(),
               "disconnect does not report a known output after losing the transport");
    } else {
        Expect(state["channels"][0]["output"] == false && state["channels"][1]["output"] == false &&
                   state["channels"][2]["output"] == true,
               "selected/owned CH1 is off, canceled CH2 ON never executes, unselected CH3 stays on");
    }
}

void TestSafetyReserveAndCoalescing() {
    PowerService power;
    const bool paused = PowerServiceTestAccess::Pause(power);
    Expect(paused, "worker pause is deterministic for reserve testing");
    if (!paused) return;
    Json first = {{"type", "parameters"}, {"channels", {1}}, {"voltage", 1}, {"current", 1},
                  {"requestId", "ordinary-0"}};
    auto original = power.Submit(first);
    for (int i = 1; i < 128; ++i) {
        auto command = first;
        command["requestId"] = "ordinary-" + std::to_string(i);
        Expect(!power.Submit(command).contains("error"), "noncancelable backlog reaches 128");
    }
    Expect(power.Submit(first).value("id", "") == original.value("id", "missing"),
           "identical request is returned without reenqueueing even when full");
    auto extra = first;
    extra["requestId"] = "ordinary-overflow";
    Expect(power.Submit(extra).contains("error"), "ordinary overflow is still rejected");
    Json previous = Json::array();

    // Seven nonempty channel sets, stop and disconnect: at most nine safety slots.
    // Reverse channel order to check semantic scope, not JSON array ordering.
    for (int repeat = 0; repeat < 40; ++repeat) {
        for (int mask = 1; mask < 8; ++mask) {
            Json channels = Json::array();
            for (int channel = 3; channel >= 1; --channel)
                if (mask & (1 << (channel - 1))) channels.push_back(channel);
            if (repeat % 2) std::reverse(channels.begin(), channels.end());
            auto off = power.Submit({{"type", "output"}, {"channels", channels}, {"enabled", false},
                                      {"source", repeat % 2 ? "local" : "network"}});
            Expect(!off.contains("error"), "each OFF scope has reserved capacity");
            if (repeat == 0) previous.push_back(off);
            if (repeat == 1 && !previous[mask - 1].contains("error"))
                Expect(power.Action(previous[mask - 1]["id"]).value("state", "") == "canceled",
                       "coalescing reports superseded OFF as canceled, never completed");
            Expect(PowerServiceTestAccess::Depth(power) <= 137, "repeated OFF cannot grow the queue without bound");
        }
        Expect(!power.Submit({{"type", "stop"}}).contains("error"), "stop has reserved capacity");
        Expect(!power.Submit({{"type", "disconnect"}}).contains("error"), "disconnect has reserved capacity");
        Expect(PowerServiceTestAccess::Depth(power) == 137, "all safety scopes coexist in a bounded queue");
    }
    Expect(power.Action(original["id"]).value("state", "") == "queued",
           "safety reserve does not evict unrelated ordinary work");
    power.Shutdown(); // Must wake a paused worker and reject all further work.
    Expect(power.Submit({{"type", "stop"}}).contains("error") && PowerServiceTestAccess::Depth(power) == 0,
           "shutdown remains authoritative and drains the test queue");
}

void TestSafetyReadbackFailureAtCapacity() {
    PowerService power;
    auto ready = Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"},
                                          {"testFailAfterReads", 33}}));
    Expect(ready.value("state", "") == "completed", "readback fault starts after initial identification");
    const bool paused = PowerServiceTestAccess::Pause(power);
    Expect(paused, "readback fault worker pauses before saturation");
    if (!paused) return;
    for (int i = 0; i < 128; ++i)
        Expect(!power.Submit({{"type", "output"}, {"channels", {2}}, {"enabled", true}}).contains("error"),
               "readback fault test fills all ordinary slots");
    auto off = power.Submit({{"type", "output"}, {"channels", {1}}, {"enabled", false}, {"source", "network"}});
    Expect(!off.contains("error"), "OFF is admitted before the readback fault occurs");
    PowerServiceTestAccess::Resume(power);
    auto failed = Wait(power, off);
    const auto state = power.State();
    Expect(failed.value("state", "") == "failed" && failed.contains("error") &&
               !state.value("connected", true) && state["channels"][0]["output"].is_null(),
           "failed OFF readback remains failed/unknown, never confirmed off");
}

int main() {
    TestSafetyAtCapacity({{"type", "output"}, {"channels", {1}}, {"enabled", false}, {"source", "local"}});
    TestSafetyAtCapacity({{"type", "output"}, {"channels", {1}}, {"enabled", false}, {"source", "network"}});
    TestSafetyAtCapacity({{"type", "stop"}, {"source", "local"}});
    TestSafetyAtCapacity({{"type", "disconnect"}, {"source", "local"}});
    TestSafetyReserveAndCoalescing();
    TestSafetyReadbackFailureAtCapacity();
    PowerService power;
    Expect(Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"}}))["state"] == "completed",
           "simulation identification and readback");
    Expect(power.Submit({{"type", "output"}, {"channels", Json::array()}, {"enabled", true}}).contains("error"),
           "empty selection rejected");
    Expect(power.Submit({{"type", "output"}, {"channels", Json::array({1, 4})}, {"enabled", true}}).contains("error"),
           "invalid batch rejected before hardware access");
    Expect(power.Submit({{"type", "parameters"}, {"channels", Json::array({3})}, {"voltage", 30}, {"current", 1}})
               .contains("error"),
           "CH3 voltage limit");
    Expect(power.Submit({{"type", "output"}, {"channels", Json::array({4294967297ull})}, {"enabled", true}})
               .contains("error"),
           "oversized channel cannot truncate to CH1");
    Expect(
        power.Submit({{"type", "task"}, {"channels", Json::array({1})}, {"onMs", 100.5}, {"offMs", 100}, {"count", 1}})
            .contains("error"),
        "fractional integer timing rejected");
    auto set = Wait(
        power,
        power.Submit({{"type", "parameters"}, {"channels", Json::array({1})}, {"voltage", 12.0}, {"current", 2.0}}));
    Expect(set["state"] == "completed" && power.State()["channels"][0]["setVoltage"] == 12.0, "parameter readback");
    Json on = {{"type", "output"}, {"channels", Json::array({1, 3})}, {"enabled", true}, {"requestId", "test-on"}};
    auto action = Wait(power, power.Submit(on));
    Expect(action["state"] == "completed", "batch output on");
    Expect(power.State()["channels"][0]["output"] == true && power.State()["channels"][1]["output"] == false &&
               power.State()["channels"][2]["output"] == true,
           "unselected CH2 untouched");
    Expect(power.Submit(on)["id"] == action["id"], "request ID deduplication");
    on["enabled"] = false;
    Expect(power.Submit(on).contains("error"), "request ID conflict rejected");
    auto task = Wait(
        power, power.Submit(
                   {{"type", "task"}, {"channels", Json::array({1, 2})}, {"onMs", 100}, {"offMs", 100}, {"count", 2}}));
    Expect(task["state"] == "completed", "task accepted");
    for (int i = 0; i < 100 && power.State()["task"].value("running", false); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto state = power.State();
    Expect(state["task"]["running"] == false && state["channels"][0]["output"] == false &&
               state["channels"][1]["output"] == false && state["channels"][2]["output"] == true,
           "completed task shuts off only its owned channels");
    Wait(power,
         power.Submit(
             {{"type", "task"}, {"channels", Json::array({2})}, {"onMs", 5000}, {"offMs", 5000}, {"count", 10}}));
    auto conflict =
        Wait(power,
             power.Submit({{"type", "output"}, {"channels", Json::array({2})}, {"enabled", true}, {"source", "api"}}));
    Expect(conflict["state"] == "failed" && power.State()["connected"] == true &&
               power.State()["task"].value("running", false),
           "remote manual ON cannot override an active local task");
    auto stop = Wait(power, power.Submit({{"type", "stop"}}));
    Expect(stop["state"] == "completed" && power.State()["channels"][1]["output"] == false,
           "stop disables owned output");
    auto protect = Wait(power, power.Submit({{"type", "protection"},
                                             {"channels", Json::array({1})},
                                             {"enabled", true},
                                             {"voltageLimit", 5.0},
                                             {"currentLimit", 2.0}}));
    Expect(protect["state"] == "completed" && power.State()["channels"][0]["protection"]["voltageLimit"] == 5.0,
           "protection hardware readback");
    Wait(power, power.Submit({{"type", "output"}, {"channels", Json::array({1})}, {"enabled", true}}));
    for (int i = 0; i < 100 && power.State()["channels"][0]["output"] == true; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Expect(power.State()["channels"][0]["output"] == false && power.State().contains("error"),
           "overvoltage sampling shuts down channel");
    Expect(power.State()["channels"][2]["output"] == true, "protection leaves unselected output alone");
    Expect(power
               .Submit({{"type", "protection"},
                        {"channels", Json::array({1, 3})},
                        {"enabled", true},
                        {"voltageLimit", 10.0},
                        {"currentLimit", 1.0}})
               .contains("error"),
           "mixed-channel protection validation is atomic");
    Wait(power,
         power.Submit(
             {{"type", "task"}, {"channels", Json::array({2, 3})}, {"onMs", 10000}, {"offMs", 10000}, {"count", 10}}));
    Wait(power, power.Submit({{"type", "output"}, {"channels", Json::array({2})}, {"enabled", false}}));
    Expect(!power.State()["task"].value("running", false) && power.State()["ownedChannels"] == Json::array({2, 3}),
           "manual off cancels future on and retains cleanup ownership");
    Wait(power, power.Submit({{"type", "stop"}}));
    Expect(power.State()["channels"][2]["output"] == false, "stop handles channels retained after manual off");
    const auto csv = power.State().value("csvPath", std::string());
    Expect(!csv.empty(), "measurement CSV created");
    power.Shutdown();
    Expect(power.State()["connected"] == false && power.State()["channels"][0]["output"].is_null(),
           "disconnected state is unknown");
    PowerService failure;
    auto fault =
        Wait(failure, failure.Submit({{"type", "connect"}, {"backend", "simulation"}, {"testFailAfterReads", 0}}));
    Expect(fault["state"] == "failed" && !failure.State()["connected"].get<bool>(),
           "query timeout does not claim connection");
    PowerService interrupted;
    auto ready = Wait(interrupted,
                      interrupted.Submit({{"type", "connect"}, {"backend", "simulation"}, {"testFailAfterReads", 33}}));
    Expect(ready["state"] == "completed", "fault injection leaves initial readback intact");
    auto partial = Wait(interrupted,
                        interrupted.Submit({{"type", "output"}, {"channels", Json::array({1, 2})}, {"enabled", true}}));
    Expect(partial["state"] == "failed" && partial["channels"][0].contains("shutdownAttempt") &&
               !interrupted.State()["connected"].get<bool>(),
           "failed energizing attempts shutdown and inhibits later writes");
    PowerService extended;
    Expect(Wait(extended, extended.Submit({{"type", "connect"}, {"backend", "simulation"}}))["state"] == "completed",
           "extended instrument setup");
    auto mode = Wait(extended, extended.Submit({{"type", "mode"}, {"mode", "parallel"}}));
    Expect(mode["state"] == "completed" && extended.State()["mode"] == "parallel", "parallel mode readback");
    auto group =
        Wait(extended,
             extended.Submit(
                 {{"type", "batch"},
                  {"mode", "parallel"},
                  {"settings",
                   Json::array({Json{{"channels", Json::array({1, 2})}, {"voltage", 12.0}, {"current", 10.0}}})}}));
    Expect(group["state"] == "completed" && extended.State()["channels"][0]["setCurrent"] == 5.0 &&
               extended.State()["channels"][1]["setCurrent"] == 5.0,
           "parallel total current divided across both channels");
    auto subset =
        Wait(extended, extended.Submit({{"type", "output"}, {"channels", Json::array({1})}, {"enabled", true}}));
    Expect(subset["state"] == "failed", "combined output rejects partial selection");
    Wait(extended, extended.Submit({{"type", "connect"}, {"backend", "simulation"}}));
    Wait(extended, extended.Submit({{"type", "mode"}, {"mode", "series"}}));
    auto series = Wait(
        extended, extended.Submit(
                      {{"type", "batch"},
                       {"mode", "series"},
                       {"settings",
                        Json::array({Json{{"channels", Json::array({1, 2})}, {"voltage", 48.0}, {"current", 2.0}}})}}));
    Expect(series["state"] == "completed" && extended.State()["channels"][0]["setVoltage"] == 24.0 &&
               extended.State()["channels"][1]["setVoltage"] == 24.0,
           "series total voltage divided across both channels");
    Wait(extended, extended.Submit({{"type", "output"}, {"channels", Json::array({1, 2})}, {"enabled", true}}));
    Wait(extended, extended.Submit({{"type", "mode"}, {"mode", "independent"}}));
    Expect(extended.State()["channels"][0]["output"] == false && extended.State()["channels"][1]["output"] == false,
           "mode switch leaves related outputs off");
    auto writes = extended.State()["testWrites"];
    auto release = std::find(writes.begin(), writes.end(), Json("INST:COM:PARA NONE"));
    Expect(release != writes.end(), "mode transitions release existing combination");
    Expect(extended
               .Submit(
                   {{"type", "batch"},
                    {"mode", "independent"},
                    {"settings", Json::array({Json{{"channels", Json::array({1})}, {"voltage", 12}, {"current", 2}},
                                              Json{{"channels", Json::array({3})}, {"voltage", 12}, {"current", 2}}})}})
               .contains("error"),
           "invalid final item rejects whole batch before any write");
    auto batch =
        Wait(extended,
             extended.Submit(
                 {{"type", "batch"},
                  {"mode", "independent"},
                  {"settings", Json::array({Json{{"channels", Json::array({1})}, {"voltage", 3}, {"current", 1}},
                                            Json{{"channels", Json::array({3})}, {"voltage", 5}, {"current", 2}}})}}));
    Expect(batch["state"] == "completed" && extended.State()["channels"][0]["setVoltage"] == 3 &&
               extended.State()["channels"][2]["setVoltage"] == 5,
           "independent settings remain distinct");
    Expect(extended.Submit({{"type", "settings"}, {"operation", "savePreset"}, {"slot", 0}}).contains("error"),
           "device storage is 1..36");
    auto preset = Wait(extended, extended.Submit({{"type", "settings"}, {"operation", "savePreset"}, {"slot", 36}}));
    Expect(preset["state"] == "completed", "device preset saved");
    auto timer = Wait(extended, extended.Submit({{"type", "deviceTimer"}, {"enabled", true}, {"seconds", 3.5}}));
    Expect(timer["state"] == "completed" && extended.State()["timer"]["seconds"] == 3.5,
           "device timer configured and read back");
    Expect(extended.Submit({{"type", "deviceTimer"}, {"enabled", true}, {"seconds", 100000}}).contains("error"),
           "device timer upper limit");
    auto panel = Wait(extended, extended.Submit({{"type", "settings"},
                                                 {"operation", "panel"},
                                                 {"control", "remote"},
                                                 {"locked", true},
                                                 {"display", false},
                                                 {"text", "SerialCtl"}}));
    Expect(panel["state"] == "completed" && extended.State()["panel"]["display"] == false, "panel screen readback");
    Expect(extended
               .Submit({{"type", "settings"},
                        {"operation", "panel"},
                        {"control", "remote"},
                        {"locked", false},
                        {"display", true},
                        {"text", "hello\";OUTP ON"}})
               .contains("error"),
           "screen text cannot inject SCPI");
    auto protection = Wait(extended, extended.Submit({{"type", "settings"},
                                                      {"operation", "protection"},
                                                      {"channels", Json::array({3})},
                                                      {"enabled", true},
                                                      {"limit", 4.5},
                                                      {"ovp", 5.5},
                                                      {"softwareVoltage", 0},
                                                      {"softwareCurrent", 0}}));
    Expect(protection["state"] == "completed" && extended.State()["channels"][2]["deviceProtection"]["ovp"] == 5.5,
           "CH3 hardware OVP distinct from software threshold");
    auto trigger = Wait(extended, extended.Submit({{"type", "trigger"},
                                                   {"channels", Json::array({1})},
                                                   {"delay", 0},
                                                   {"voltageStep", 0.1},
                                                   {"currentStep", 0.1},
                                                   {"voltage", 4},
                                                   {"current", 1},
                                                   {"fire", true}}));
    Expect(trigger["state"] == "completed" && extended.State()["channels"][0]["setVoltage"] == 4,
           "triggered parameter readback");
    auto step = Wait(
        extended,
        extended.Submit({{"type", "step"}, {"channels", Json::array({1})}, {"quantity", "voltage"}, {"direction", 1}}));
    Expect(step["state"] == "completed" &&
               std::abs(extended.State()["channels"][0]["setVoltage"].get<double>() - 4.1) < 0.001,
           "step uses device step size and verifies new setpoint");
    auto sequence = Wait(
        extended,
        extended.Submit(
            {{"type", "sequence"},
             {"channels", Json::array({1})},
             {"steps", Json::array({Json{{"voltage", 2}, {"current", 1}, {"durationMs", 100}, {"enabled", true}},
                                    Json{{"voltage", 3}, {"current", 1}, {"durationMs", 100}, {"enabled", false}}})}}));
    Expect(sequence["state"] == "completed", "sequence accepted");
    for (int i = 0; i < 100 && extended.State()["task"].value("running", false); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Expect(!extended.State()["task"].value("running", false) && extended.State()["channels"][0]["output"] == false &&
               extended.State()["channels"][0]["setVoltage"] == 3,
           "sequence advances and powers off on completion");
    Wait(extended, extended.Submit(
                       {{"type", "schedule"}, {"channels", Json::array({1})}, {"startMs", 200}, {"durationMs", 200}}));
    Wait(extended, extended.Submit({{"type", "stop"}}));
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    Expect(extended.State()["channels"][0]["output"] == false && !extended.State()["task"].value("running", false),
           "canceling scheduled task prevents later energizing");
    Expect(
        extended.Submit({{"type", "diagnostic"}, {"operation", "scpi"}, {"line", "*IDN?\nOUTP ON"}}).contains("error"),
        "SCPI debug rejects multiple instructions");
    Wait(extended, extended.Submit({{"type", "mode"}, {"mode", "parallel"}}));
    auto combinedSequence = Wait(
        extended,
        extended.Submit(
            {{"type", "sequence"},
             {"channels", Json::array({1, 2, 3})},
             {"steps", Json::array({Json{{"voltage", 2}, {"current", 2}, {"durationMs", 100}, {"enabled", true}}})}}));
    Expect(combinedSequence["state"] == "completed", "combined group and CH3 sequence accepted");
    for (int i = 0; i < 100 && extended.State()["task"].value("running", false); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Expect(extended.State()["connected"] == true && extended.State()["channels"][0]["setCurrent"] == 1 &&
               extended.State()["channels"][1]["setCurrent"] == 1 && extended.State()["channels"][2]["setCurrent"] == 2,
           "sequence partitions combined total and independent CH3 parameters");
    Expect(!extended.State()["task"].value("running", false) && extended.State()["channels"][2]["output"] == false,
           "combined sequence completion powers off entire owned scope");
    Expect(!extended.State()["records"].empty(), "bounded action history includes local operations");
    return failures ? 1 : 0;
}
