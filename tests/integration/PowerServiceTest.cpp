#include "PowerService.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>
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
int main() {
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
