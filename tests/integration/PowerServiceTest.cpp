#include "PowerService.h"
#include <iostream>
#include <thread>
using namespace serialctl;
int failures = 0;
void Expect(bool ok, const char *message)
{
    if (!ok)
    {
        std::cerr << "FAIL " << message << "\n";
        ++failures;
    }
}
Json Wait(PowerService &power, Json action)
{
    if (action.contains("error"))
        return action;
    for (int i = 0; i < 100; ++i)
    {
        auto result = power.Action(action["id"]);
        if (result["state"] != "queued" && result["state"] != "running")
            return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {{"error", "Timeout"}};
}
int main()
{
    PowerService power;
    Expect(Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"}}))["state"] ==
               "completed",
           "simulation identification and readback");
    Expect(power.Submit({{"type", "output"}, {"channels", Json::array()}, {"enabled", true}})
               .contains("error"),
           "empty selection rejected");
    Expect(power.Submit({{"type", "output"}, {"channels", Json::array({1, 4})}, {"enabled", true}})
               .contains("error"),
           "invalid batch rejected before hardware access");
    Expect(power
               .Submit({{"type", "parameters"},
                        {"channels", Json::array({3})},
                        {"voltage", 30},
                        {"current", 1}})
               .contains("error"),
           "CH3 voltage limit");
    Expect(
        power
            .Submit(
                {{"type", "output"}, {"channels", Json::array({4294967297ull})}, {"enabled", true}})
            .contains("error"),
        "oversized channel cannot truncate to CH1");
    Expect(power
               .Submit({{"type", "task"},
                        {"channels", Json::array({1})},
                        {"onMs", 100.5},
                        {"offMs", 100},
                        {"count", 1}})
               .contains("error"),
           "fractional integer timing rejected");
    auto set = Wait(power, power.Submit({{"type", "parameters"},
                                         {"channels", Json::array({1})},
                                         {"voltage", 12.0},
                                         {"current", 2.0}}));
    Expect(set["state"] == "completed" && power.State()["channels"][0]["setVoltage"] == 12.0,
           "parameter readback");
    Json on = {{"type", "output"},
               {"channels", Json::array({1, 3})},
               {"enabled", true},
               {"requestId", "test-on"}};
    auto action = Wait(power, power.Submit(on));
    Expect(action["state"] == "completed", "batch output on");
    Expect(power.State()["channels"][0]["output"] == true &&
               power.State()["channels"][1]["output"] == false &&
               power.State()["channels"][2]["output"] == true,
           "unselected CH2 untouched");
    Expect(power.Submit(on)["id"] == action["id"], "request ID deduplication");
    on["enabled"] = false;
    Expect(power.Submit(on).contains("error"), "request ID conflict rejected");
    auto task = Wait(power, power.Submit({{"type", "task"},
                                          {"channels", Json::array({1, 2})},
                                          {"onMs", 100},
                                          {"offMs", 100},
                                          {"count", 2}}));
    Expect(task["state"] == "completed", "task accepted");
    for (int i = 0; i < 100 && power.State()["task"].value("running", false); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto state = power.State();
    Expect(state["task"]["running"] == false && state["channels"][0]["output"] == false &&
               state["channels"][1]["output"] == false && state["channels"][2]["output"] == true,
           "completed task shuts off only its owned channels");
    Wait(power, power.Submit({{"type", "task"},
                              {"channels", Json::array({2})},
                              {"onMs", 5000},
                              {"offMs", 5000},
                              {"count", 10}}));
    auto stop = Wait(power, power.Submit({{"type", "stop"}}));
    Expect(stop["state"] == "completed" && power.State()["channels"][1]["output"] == false,
           "stop disables owned output");
    auto protect = Wait(power, power.Submit({{"type", "protection"},
                                             {"channels", Json::array({1})},
                                             {"enabled", true},
                                             {"voltageLimit", 5.0},
                                             {"currentLimit", 2.0}}));
    Expect(protect["state"] == "completed" &&
               power.State()["channels"][0]["protection"]["voltageLimit"] == 5.0,
           "protection hardware readback");
    Wait(power,
         power.Submit({{"type", "output"}, {"channels", Json::array({1})}, {"enabled", true}}));
    for (int i = 0; i < 100 && power.State()["channels"][0]["output"] == true; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Expect(power.State()["channels"][0]["output"] == false && power.State().contains("error"),
           "overvoltage sampling shuts down channel");
    Expect(power.State()["channels"][2]["output"] == true,
           "protection leaves unselected output alone");
    Expect(power
               .Submit({{"type", "protection"},
                        {"channels", Json::array({1, 3})},
                        {"enabled", true},
                        {"voltageLimit", 10.0},
                        {"currentLimit", 1.0}})
               .contains("error"),
           "mixed-channel protection validation is atomic");
    Wait(power, power.Submit({{"type", "task"},
                              {"channels", Json::array({2, 3})},
                              {"onMs", 10000},
                              {"offMs", 10000},
                              {"count", 10}}));
    Wait(power,
         power.Submit({{"type", "output"}, {"channels", Json::array({2})}, {"enabled", false}}));
    Expect(!power.State()["task"].value("running", false) &&
               power.State()["ownedChannels"] == Json::array({2, 3}),
           "manual off cancels future on and retains cleanup ownership");
    Wait(power, power.Submit({{"type", "stop"}}));
    Expect(power.State()["channels"][2]["output"] == false,
           "stop handles channels retained after manual off");
    const auto csv = power.State().value("csvPath", std::string());
    Expect(!csv.empty(), "measurement CSV created");
    power.Shutdown();
    Expect(power.State()["connected"] == false && power.State()["channels"][0]["output"].is_null(),
           "disconnected state is unknown");
    PowerService failure;
    auto fault = Wait(
        failure, failure.Submit(
                     {{"type", "connect"}, {"backend", "simulation"}, {"testFailAfterReads", 0}}));
    Expect(fault["state"] == "failed" && !failure.State()["connected"].get<bool>(),
           "query timeout does not claim connection");
    PowerService interrupted;
    auto ready = Wait(interrupted, interrupted.Submit({{"type", "connect"},
                                                       {"backend", "simulation"},
                                                       {"testFailAfterReads", 27}}));
    Expect(ready["state"] == "completed", "fault injection leaves initial readback intact");
    auto partial = Wait(interrupted, interrupted.Submit({{"type", "output"},
                                                         {"channels", Json::array({1, 2})},
                                                         {"enabled", true}}));
    Expect(partial["state"] == "failed" && partial["channels"][0].contains("shutdownAttempt") &&
               !interrupted.State()["connected"].get<bool>(),
           "failed energizing attempts shutdown and inhibits later writes");
    return failures ? 1 : 0;
}
