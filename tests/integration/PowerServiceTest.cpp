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
    static void FailNextOutputReadback(PowerService &power) {
        power.FailNextOutputReadbackForTest();
    }
    static void FailNextOutputOffWrite(PowerService &power) {
        power.FailNextOutputOffWriteForTest();
    }
    static void PauseRecoveryOn(PowerService &power) {
        std::lock_guard<std::mutex> lock(power.mutex_);
        power.testPauseRecoveryOn_=true;power.testRecoveryAtOn_=false;
    }
    static bool WaitRecoveryOn(PowerService &power) {
        std::unique_lock<std::mutex> lock(power.mutex_);
        return power.wake_.wait_for(lock,std::chrono::seconds(15),[&]{return power.testRecoveryAtOn_;});
    }
    static void ResumeRecoveryOn(PowerService &power) {
        std::lock_guard<std::mutex> lock(power.mutex_);
        power.testPauseRecoveryOn_=false;power.wake_.notify_all();
    }
    static Json PreserveDisconnectBeforePublishing(PowerService &power) {
        power.Shutdown();
        Json disconnect{{"id", "disconnect"}, {"state", "running"}};
        power.actions_.emplace("disconnect", disconnect);
        power.actions_.emplace("recovery", Json{{"id", "recovery"}, {"state", "running"}});
        power.Disconnect(true, &disconnect);
        return disconnect;
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

template <typename Predicate> bool WaitForKeywordTest(Predicate ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

void TestKeywordCycle() {
    PowerService power;
    Expect(Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"}})).value("state", "") == "completed",
           "Connect keyword test fixture");
    using Run = PowerKeywordRun;
    std::vector<Run::Rule> rules{
        {{L"READY", L"go", L"", L"START", L"END", 10, 0}, "READY", "START", "END", {'g', 'o'}, {}},
        {{L"LINK", L"", L"diag", L"START", L"END"}, "LINK", "START", "END", {}, {'d', 'i', 'a', 'g'}},
        {{L"LOGIN", L"", L"unexpected", L"LOGIN WINDOW", L""}, "LOGIN", "LOGIN WINDOW", "", {}, {'x'}},
        {{L"ROUND-MISS", L"", L"check"}, "ROUND-MISS", "", "", {}, {'c', 'h', 'e', 'c', 'k'}}
    };
    std::mutex senderMutex;
    std::vector<std::string> sent;
    Run::Completion firstWrite;
    const auto run = std::make_shared<Run>(1, L"COM fixture", "READY", rules,
        [&](const Bytes &bytes, const std::function<bool()> &allowed, Run::Completion completion, std::wstring &) {
            if (!allowed()) return false;
            std::lock_guard<std::mutex> guard(senderMutex);
            sent.emplace_back(bytes.begin(), bytes.end());
            if (sent.size() == 1) firstWrite = std::move(completion);
            else completion(Run::Delivery::Written, {});
            return true;
        }, [] { return true; }, [](const std::wstring &) {});
    const auto feed = [&](const std::string &bytes) { run->Feed(Bytes(bytes.begin(), bytes.end())); };
    feed("READY START READY END");
    const Json cycle{{"type", "task"}, {"channels", {1}}, {"onMs", 500}, {"offMs", 100}, {"count", 2}};
    Expect(Wait(power, power.Submit(cycle, {}, {}, run)).value("state", "") == "completed",
           "Accept a local keyword cycle");
    Expect(WaitForKeywordTest([&] { return run->Status().state == Run::State::Listening; }), "Open the first detection window");
    feed("REA");
    feed("DY STA");
    feed("RT RE");
    feed("ADY READY EN");
    feed("D READY");
    auto snapshot = run->Status();
    Expect(snapshot.ready && snapshot.counts[0].round == 2 && snapshot.counts[0].window == Run::Window::Closed,
           "Detect split markers and repeated hits only in the configured window");
    Expect(snapshot.counts[1].window == Run::Window::Closed && snapshot.counts[2].window == Run::Window::Waiting,
           "Distinguish zero hits from a detection window that never opens");
    Expect(WaitForKeywordTest([&] { return run->Status().state == Run::State::Draining; }),
           "End detection while waiting for terminal writes");
    Expect(power.State()["channels"][0]["output"] == true, "Do not power off while the terminal write is still in flight");
    {
        std::lock_guard<std::mutex> guard(senderMutex);
        if (firstWrite) firstWrite(Run::Delivery::Written, {});
        else Expect(false, "First action reached the transport");
    }
    Expect(WaitForKeywordTest([&] {
        const auto second = run->Status();
        return second.round == 2 && second.state == Run::State::Listening;
    }), "Open a new window at the next power-on");
    feed("START READY LINK END");
    Expect(WaitForKeywordTest([&] { return run->Status().state == Run::State::Finished; }), "Finish both power rounds");
    snapshot = run->Status();
    Expect(snapshot.counts[0].round == 1 && snapshot.counts[0].total == 3 && snapshot.counts[0].written == 3,
           "Reset per-round counts and retain cumulative captures and writes");
    Expect(snapshot.counts[1].written == 1 && snapshot.counts[2].written == 0 && snapshot.counts[3].written == 2,
           "Execute misses at marker or round end, only for windows that opened");
    {
        std::lock_guard<std::mutex> guard(senderMutex);
        Expect(sent == std::vector<std::string>({"go", "go", "diag", "check", "go", "check"}),
               "Send every hit and miss in capture order");
    }
    Expect(power.State()["channels"][0]["output"] == false, "Normal completion confirms output OFF");

    rules[0].display.hitDelayMs = 2000;
    const auto canceled = std::make_shared<Run>(1, L"COM fixture", "READY", rules,
        [](const Bytes &, const std::function<bool()> &, Run::Completion, std::wstring &) { return false; },
        [] { return true; }, [](const std::wstring &) {});
    Expect(Wait(power, power.Submit(cycle, {}, {}, canceled)).value("state", "") == "completed", "Start cancel fixture");
    Expect(WaitForKeywordTest([&] { return canceled->Status().state == Run::State::Listening; }), "Listen before cancel");
    canceled->Feed(Bytes{'S','T','A','R','T',' ','R','E','A','D','Y'});
    const auto stop = power.Submit({{"type", "stop"}});
    Expect(!canceled->Active() && canceled->Status().counts[0].canceled == 1,
           "Accepting Stop immediately revokes a delayed command");
    Expect(Wait(power, stop).value("state", "") == "completed", "Stop turns the test outputs off");
}

void TestBoundRemoteCommands() {
    PowerService power;
    const Json connect{{"type", "connect"}, {"backend", "simulation"}};
    Expect(Wait(power, power.Submit(connect)).value("state", "") == "completed", "Connect remote power fixture");
    const auto connection = power.State().at("connectionId").get<std::string>();
    const Json remoteOn{{"type", "output"}, {"channels", {1}}, {"enabled", true}, {"source", "remote-power:1"}};
    Expect(power.Submit(remoteOn, connection, "series").contains("error"), "Reject a stale remote mode");
    const bool paused = PowerServiceTestAccess::Pause(power);
    const auto remote = power.Submit(remoteOn, connection, "independent");
    const auto local = power.Submit({{"type", "output"}, {"channels", {2}}, {"enabled", true}});
    power.CancelRemotePending("remote-power:1");
    PowerServiceTestAccess::Resume(power);
    Expect(paused && Wait(power, remote).value("state", "") == "canceled", "Release cancels only queued remote work");
    Expect(Wait(power, local).value("state", "") == "completed" && power.State()["channels"][1]["output"] == true,
           "Local controls still operate the physical connection");
    const auto disconnect = Wait(power, power.Submit({{"type", "disconnect"}, {"preserveOutputs", true}}));
    if (disconnect.value("state", "") != "completed") std::cerr << "Disconnect receipt: " << disconnect.dump() << '\n';
    Expect(disconnect.value("state", "") == "completed" && disconnect.value("transportDisconnected", false),
           "Disconnect fixture before reconnect");
    Expect(Wait(power, power.Submit(connect)).value("state", "") == "completed", "Reconnect fixture");
    Expect(power.Submit(remoteOn, connection, "independent").contains("error"), "Reject commands bound to the old connection");
}

void TestPreserveDisconnectPublication() {
    PowerService power;
    const auto result = PowerServiceTestAccess::PreserveDisconnectBeforePublishing(power);
    Expect(power.Action("disconnect").value("state", "") == "running",
           "Preserving outputs keeps the disconnect action running until its final receipt is published");
    Expect(power.Action("recovery").value("state", "") == "canceled",
           "Preserving outputs cancels other running work");
    Expect(result.value("state", "") == "completed" && result.value("offConfirmation", "") == "preserved",
           "The final disconnect receipt reports preserved outputs");
}

void TestConnectScpiDiagnostics() {
    const std::vector<std::pair<std::string, std::string>> scenarios = {
        {"*IDN?", "identify"},
        {"INST:NSEL?", "select_channel"},
        {"OUTP:TRAC?", "read_mode"},
        {"MEAS:VOLT?", "initial_poll"}
    };
    for (const auto &scenario : scenarios) {
        PowerService power;
        const auto result = Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"},
                                                      {"testFailQuery", scenario.first}}));
        const auto error = result.value("error", std::string());
        Expect(result.value("state", "") == "failed" && result.value("stage", "") == scenario.second &&
                   error.find("Power connect [" + scenario.second + "]") != std::string::npos &&
                   error.find(scenario.first) != std::string::npos,
               "Failed connect identifies the precise stage and SCPI query");
        const auto state = power.State();
        Expect(!state.value("connected", false), "Failed connection releases transport");
        const auto diagnostic = state.value("lastConnectDiagnostic", Json::object());
        Expect(diagnostic.value("stage", "") == scenario.second && diagnostic.value("error", "") == error,
               "Failed connection retains accessible diagnostics");
        bool hasError = false;
        for (const auto &row : result.value("scpiTrace", Json::array())) {
            if (row.value("direction", "") == "ERROR" && row.value("command", "") == scenario.first) hasError = true;
            Expect(row.value("command", "") != "*RST" && row.value("command", "") != "CHAN:OUTP ON",
                   "Connection failure does not reset or enable supply");
        }
        Expect(hasError, "Failed query is present in SCPI trace");
    }
    {
        PowerService power;
        const auto result = Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"},
                                                      {"testFailRemoteWrite", true}}));
        Expect(result.value("state", "") == "failed" && result.value("stage", "") == "enter_remote" &&
                   result.value("error", std::string()).find("remote-mode write failure") != std::string::npos,
               "Remote-control write errors are reported at the correct stage");
        Expect(!power.State().value("connected", false), "Failed remote-mode initialization disconnects");
    }
    {
        PowerService power;
        const auto result = Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"}}));
        Expect(result.value("state", "") == "completed", "Simulated power connection completes");
        const auto trace = result.value("scpiTrace", Json::array());
        auto position = [&](const std::string &command) {
            size_t index = 0;
            for (const auto &row : trace) {
                if (row.value("direction", "") == "TX" && row.value("command", "") == command) return index;
                ++index;
            }
            return trace.size();
        };
        Expect(position("*IDN?") < position("SYST:REM") &&
                   position("SYST:REM") < position("INST:NSEL 1") &&
                   position("INST:NSEL 1") < position("OUTP:TRAC?"),
               "Identity precedes remote-control request and subsequent queries");
        Expect(power.State().value("connected", false) &&
                   power.State().value("lastConnectDiagnostic", Json::object()).value("stage", "") == "complete",
               "Successful connection retains diagnostics");
        for (const auto &row : trace)
            Expect(row.value("command", "") != "*RST" && row.value("command", "") != "CHAN:OUTP ON",
                   "Connection neither resets nor enables the output");
    }
}

void TestNativeRecoveryTimingAndCancel() {
    PowerService power;
    Expect(Wait(power,power.Submit({{"type","connect"},{"backend","simulation"}})).value("state","")=="completed", "Recovery fixture uses controlled device transport only");
    const auto ready=power.State();
    auto recovery=power.Submit({{"type","recover"},{"channels",{1}},{"identity",ready["identity"]},{"mode",ready["mode"]}});
    Json result;
    for(int i=0;i<300;++i) {
        result=power.Action(recovery.at("id"));
        if(result.value("state","")=="completed" || result.value("state","")=="failed") break;
        if(result.value("phase","")=="off_wait") {
            const auto observed=power.State();
            // Action and State are separate snapshots. The worker may legally
            // submit ON after the deadline between these reads. Only assert
            // OFF for a state observed before that deadline; final submission
            // timestamps below still verify the complete >=10000ms interval.
            if(GetTickCount64()<result.at("notBeforeOn").get<std::uint64_t>())
                Expect(observed["channels"][0]["output"]==false,"Confirmed-OFF channel stays off throughout native wait");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Expect(result.value("state","")=="completed" && result.contains("onSubmittedAt") && result.contains("offConfirmedAt") &&
        result["onSubmittedAt"].get<std::uint64_t>()-result["offConfirmedAt"].get<std::uint64_t>()>=10000,
        "Native recovery never submits ON before 10000ms after actual OFF readback");
    Expect(power.State()["channels"][0]["output"]==true && power.State()["channels"][1]["output"]==false,
        "Recovery readback energizes only the selected channel");
    auto canceled=power.Submit({{"type","recover"},{"channels",{1}},{"identity",ready["identity"]},{"mode",ready["mode"]}});
    for(int i=0;i<100 && power.Action(canceled.at("id")).value("phase","")!="off_wait";++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Expect(Wait(power,power.Submit({{"type","stop"}})).value("state","")=="completed", "Stop cancels pending recovery");
    Expect(power.Action(canceled.at("id")).value("state","")=="canceled" && power.State()["channels"][0]["output"]==false,
        "Stopped recovery remains OFF and reports canceled rather than completed");
    auto early=Wait(power,power.Submit({{"type","output"},{"channels",{1}},{"enabled",true}}));
    Expect(early.value("state","")=="failed" && power.State().value("connected",false) && power.State()["channels"][0]["output"]==false,
        "Even a direct ON cannot bypass the OFF interval; harmless rejection keeps transport alive");
}

void TestStopDuringRecoveryReadback() {
    PowerService power;
    Expect(Wait(power,power.Submit({{"type","connect"},{"backend","simulation"}})).value("state","")=="completed","Readback-race fixture connects controlled transport");
    const auto ready=power.State();
    PowerServiceTestAccess::PauseRecoveryOn(power);
    auto recovery=power.Submit({{"type","recover"},{"channels",{1}},{"identity",ready["identity"]},{"mode",ready["mode"]}});
    const bool paused=PowerServiceTestAccess::WaitRecoveryOn(power);
    Expect(paused,"Worker reaches native post-select readback boundary after the full OFF interval");
    auto stop=power.Submit({{"type","stop"}});
    Expect(!stop.contains("error") && stop.value("state","")=="queued","Stop accepts immediately while device worker is inside output preparation");
    PowerServiceTestAccess::ResumeRecoveryOn(power);
    Expect(Wait(power,stop).value("state","")=="completed","Queued stop completes after readback resumes");
    const auto action=power.Action(recovery.at("id"));
    Expect(action.value("state","")=="canceled" && !action.contains("onStartedAt"),"Canceled recovery must never reach an ON submission");
    const auto state=power.State();
    Expect(state.value("connected",false) && state["channels"][0]["output"]==false,"Readback-time stop leaves the device connected and confirmed OFF");
    for(const auto &line : state.value("testWrites",Json::array()))
        Expect(line!="CHAN:OUTP ON","Actual simulated SCPI trace contains no ON after readback-time stop");
}

void TestQueuedRecoveryCanceledBeforeStart() {
    PowerService power;
    Expect(Wait(power,power.Submit({{"type","connect"},{"backend","simulation"}})).value("state","")=="completed","Queued-cancel fixture connects controlled transport");
    const auto ready=power.State();
    const bool paused=PowerServiceTestAccess::Pause(power);
    Expect(paused,"Worker pauses before the recovery is executed");
    if(!paused) return;
    auto recovery=power.Submit({{"type","recover"},{"channels",{1}},{"identity",ready["identity"]},{"mode",ready["mode"]}});
    Expect(!recovery.contains("error") && recovery.value("state","")=="queued","Recovery waits in the queue while the worker is paused");
    // A mode change invalidates the recovery epoch at acceptance but, unlike
    // stop, leaves the recover command queued, so the worker really reaches
    // "Recovery canceled before start" when it dequeues the stale recovery.
    auto mode=power.Submit({{"type","mode"},{"mode","independent"}});
    Expect(!mode.contains("error"),"Epoch-invalidating command is accepted behind the queued recovery");
    PowerServiceTestAccess::Resume(power);
    const auto action=Wait(power,recovery);
    Expect(action.value("state","")=="canceled","Recovery canceled before start reports canceled, not failed");
    Expect(action.value("error","")=="Recovery canceled before start","Cancellation reason is the pre-start epoch check");
    Expect(!action.contains("onStartedAt"),"Recovery canceled before start never reaches ON");
    Wait(power,mode);
    const auto state=power.State();
    Expect(!state.contains("error"),"Canceled recovery does not pollute the global error state");
    Expect(state["channels"][0]["output"]==false,"Recovery canceled before start leaves the channel OFF");
    for(const auto &line : state.value("testWrites",Json::array()))
        Expect(line!="CHAN:OUTP ON","Canceled queued recovery issues no ON command");
    // Stop cancels a queued recovery at acceptance; the final state is canceled too.
    const bool pausedAgain=PowerServiceTestAccess::Pause(power);
    Expect(pausedAgain,"Worker pauses again for the stop variant");
    if(!pausedAgain) return;
    auto second=power.Submit({{"type","recover"},{"channels",{1}},{"identity",ready["identity"]},{"mode",ready["mode"]}});
    auto stop=power.Submit({{"type","stop"}});
    Expect(!second.contains("error") && !stop.contains("error"),"Recovery and stop are both accepted while paused");
    PowerServiceTestAccess::Resume(power);
    Expect(Wait(power,stop).value("state","")=="completed","Stop completes after the worker resumes");
    Expect(power.Action(second.at("id")).value("state","")=="canceled","Stop leaves the queued recovery canceled");
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
    auto ready = Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"}}));
    Expect(ready.value("state", "") == "completed", "readback fault starts after initial identification");
    const bool paused = PowerServiceTestAccess::Pause(power);
    Expect(paused, "readback fault worker pauses before saturation");
    if (!paused) return;
    PowerServiceTestAccess::FailNextOutputReadback(power);
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

void TestDisconnectOutputConfirmation(const std::string &scenario) {
    PowerService power;
    Expect(Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"}})).value("state", "") == "completed",
           "Connected simulation for disconnect test");
    Expect(Wait(power, power.Submit({{"type", "output"}, {"channels", {1, 2}}, {"enabled", true}})).value("state", "") == "completed",
           "Both owned and unowned channel start with verified ON readback");
    Expect(Wait(power, power.Submit({{"type", "schedule"}, {"channels", {1}}, {"startMs", 60000}, {"durationMs", 200}})).value("state", "") == "completed",
           "Scheduled task owns CH1 only");
    const bool paused = PowerServiceTestAccess::Pause(power);
    Expect(paused, "paused worker for disconnect fault injection");
    if (!paused) return;
    const auto before = power.State();
    if (scenario == "write") PowerServiceTestAccess::FailNextOutputOffWrite(power);
    if (scenario == "readback") PowerServiceTestAccess::FailNextOutputReadback(power);
    const bool preserve = scenario == "preserved";
    auto disconnect = power.Submit({{"type", "disconnect"}, {"source", "local"}, {"preserveOutputs", preserve}});
    PowerServiceTestAccess::Resume(power);
    const auto result = Wait(power, disconnect);
    const auto state = power.State();
    Expect(result.value("transportDisconnected", false) == true, "transportDisconnected is true");
    Expect(!state.value("connected", true), "power transport disconnected");
    const auto writes = state.value("testWrites", Json::array());
    auto selected = std::string();
    unsigned offWrites = 0;
    for (size_t index = before.value("testWrites", Json::array()).size(); index < writes.size(); ++index) {
        const auto line = writes[index].get<std::string>();
        if (line.rfind("INST:NSEL ", 0) == 0) selected = line;
        if (line == "CHAN:OUTP OFF") {
            Expect(selected == "INST:NSEL 1", "Disconnect never sends OFF to an unowned channel");
            ++offWrites;
        }
    }
    const auto offResults = result.value("offResults", Json::array());
    std::cout << "disconnect " << scenario << ": " << result.dump() << ", outputs="
              << Json::array({state["channels"][0]["output"], state["channels"][1]["output"], state["channels"][2]["output"]}).dump()
              << ", offWrites=" << offWrites << '\n';
    if (scenario == "write" || scenario == "readback") {
        Expect(result.value("state", "") == "failed" && result.value("offConfirmation", "") == "failed",
               "OFF write and readback failures leave the disconnect action failed");
        Expect(result.value("unconfirmedChannels", Json::array()) == Json::array({1}), "Only owned CH1 has an unconfirmed OFF");
        Expect(offResults.size() == 1 && offResults[0]["state"] == "unknown" &&
               offResults[0]["error"] ==
                   (scenario == "write" ? "SCPI command 'CHAN:OUTP OFF': Simulated OFF write failure"
                                        : "SCPI query 'CHAN:OUTP?': Simulated output readback timeout"),
               "Disconnect action retains the actual stage and error");
        Expect(offWrites == 2 && state["channels"][0]["output"].is_null(), "Safety retry writes OFF without claiming readback");
        const auto repeated = Wait(power, power.Submit({{"type", "disconnect"}}));
        Expect(repeated.value("offConfirmation", "") == "not_required" && power.State().value("unconfirmedPowerOff", false),
               "Repeating disconnect cannot clear an unconfirmed OFF");
        const auto unread = Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"}, {"testFailAfterReads", 6}}));
        Expect(unread.value("state", "") == "failed" && power.State().value("unconfirmedPowerOff", false) &&
               power.State()["unconfirmedPowerOffChannels"] == Json::array({1}), "Reconnect that fails readback cannot clear the warning");
        const auto read = Wait(power, power.Submit({{"type", "connect"}, {"backend", "simulation"}}));
        Expect(read.value("state", "") == "completed" && !power.State().value("unconfirmedPowerOff", true),
               "Successful hardware readback clears the unconfirmed OFF warning");
    } else if (preserve) {
        Expect(result.value("state", "") == "completed" && result.value("offConfirmation", "") == "preserved" &&
               offResults.empty() && offWrites == 0 && state.value("readbackStale", false) &&
               state["channels"][0]["output"] == true && state["channels"][1]["output"] == true,
               "Preserve outputs does not write OFF and retains both last readings");
    } else {
        Expect(result.value("state", "") == "completed" && result.value("offConfirmation", "") == "confirmed" &&
               offResults.size() == 1 && offResults[0]["state"] == "confirmed" && offResults[0]["output"] == false && offWrites == 1,
               "Successful disconnect writes and confirms OFF for owned CH1 only");
    }
}

int main() {
    TestKeywordCycle();
    TestBoundRemoteCommands();
    TestPreserveDisconnectPublication();
    TestConnectScpiDiagnostics();
    for (const auto *scenario : {"write", "readback", "confirmed", "preserved"})
        TestDisconnectOutputConfirmation(scenario);
    TestNativeRecoveryTimingAndCancel();
    TestStopDuringRecoveryReadback();
    TestQueuedRecoveryCanceledBeforeStart();
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
