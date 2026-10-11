#include "json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
using Json=nlohmann::json;
using Bytes=std::vector<std::uint8_t>;
void Require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
Json ValidateChannels(const Json &command) {
    Require(command.contains("channels") && command["channels"].is_array() && !command["channels"].empty() &&
                command["channels"].size() <= 3,
            "Select channels 1..3");
    Json channels = command["channels"];
    std::set<int> unique;
    for (const auto &item : channels) {
        Require(item.is_number_integer() && item >= 1 && item <= 3, "Invalid channel");
        int channel = item.get<int>();
        Require(channel >= 1 && channel <= 3 && unique.insert(channel).second, "Invalid or duplicate channel");
    }
    return channels;
}
struct PowerService {
std::mutex mutex_; std::condition_variable wake_;
std::deque<std::pair<std::string,Json>> queue_; std::map<std::string,Json> actions_;
bool stopping_=false; std::uint64_t next_=1;
Json State(){return {{"mode","independent"}};}
Json Submit(const Json&);
};
Json PowerService::Submit(const Json &command) {
    try {
        const std::string type = command.at("type").get<std::string>();
        Require(type == "connect" || type == "disconnect" || type == "output" || type == "parameters" ||
                    type == "protection" || type == "task" || type == "stop" || type == "mode" || type == "batch" ||
                    type == "settings" || type == "deviceTimer" || type == "trigger" || type == "diagnostic" ||
                    type == "sequence" || type == "schedule" || type == "interval" || type == "step",
                "Unknown power action");
        if (type == "output" || type == "parameters" || type == "protection" || type == "task") {
            auto channels = ValidateChannels(command);
            if (type == "output")
                Require(command.at("enabled").is_boolean(), "Invalid output state");
            if (type == "parameters")
                for (const auto &c : channels) {
                    Require(command.at("voltage").is_number() && command.at("current").is_number(),
                            "Invalid voltage or current");
                    double v = command.at("voltage"), a = command.at("current");
                    int n = c.get<int>();
                    Require(std::isfinite(v) && std::isfinite(a) && v >= 0 && a >= 0 && v <= (n == 3 ? 5 : 30) &&
                                a <= (n == 3 ? 3 : 6),
                            "Voltage or current outside channel range");
                }
            if (type == "protection") {
                Require(command.at("enabled").is_boolean(), "Invalid protection state");
                for (const auto &c : channels) {
                    int n = c.get<int>();
                    double v = command.at("voltageLimit"), a = command.at("currentLimit");
                    Require(std::isfinite(v) && std::isfinite(a) && v > 0 && a > 0 && v <= (n == 3 ? 5 : 30) &&
                                a <= (n == 3 ? 3 : 6),
                            "Invalid protection thresholds");
                }
            }
            if (type == "task") {
                Require(command.at("onMs").is_number_integer() && command.at("offMs").is_number_integer() &&
                            command.at("count").is_number_integer(),
                        "Invalid timing/count type");
                Require(command.at("onMs") >= 100 && command.at("onMs") <= 604800000 && command.at("offMs") >= 100 &&
                            command.at("offMs") <= 604800000 && command.at("count") >= 1 &&
                            command.at("count") <= 1000000,
                        "Invalid task timings/count");
            }
        }
        auto bounded = [&](const char *key, double low, double high) {
            Require(command.at(key).is_number(), "Expected numeric value");
            double n = command.at(key);
            Require(std::isfinite(n) && n >= low && n <= high, "Value outside range");
        };
        if (type == "mode")
            Require(command.at("mode") == "independent" || command.at("mode") == "parallel" ||
                        command.at("mode") == "series" || command.at("mode") == "tracking",
                    "Unknown channel mode");
        if (type == "batch") {
            Require(command.at("settings").is_array() && !command["settings"].empty() &&
                        command["settings"].size() <= 3,
                    "Invalid parameter batch");
            std::set<int> ids;
            for (const auto &item : command["settings"]) {
                auto channels = ValidateChannels(item);
                Require(item.at("voltage").is_number() && item.at("current").is_number(), "Invalid parameters");
                double v = item["voltage"], a = item["current"];
                auto mode = command.value("mode", std::string("independent"));
                bool group = channels == Json::array({1, 2}) && mode != "independent";
                for (const auto &ch : channels) {
                    int n = ch;
                    Require(ids.insert(n).second, "Duplicate parameter channel");
                    Require(std::isfinite(v) && std::isfinite(a) && v >= 0 && a >= 0 &&
                                v <= (group && mode == "series" ? 60
                                      : n == 3                  ? 5
                                                                : 30) &&
                                a <= (group && mode == "parallel" ? 12
                                      : n == 3                    ? 3
                                                                  : 6),
                            "Parameter outside range");
                }
            }
        }
        if (type == "step") {
            ValidateChannels(command);
            Require(command.at("quantity") == "voltage" || command["quantity"] == "current", "Invalid step quantity");
            Require(command.at("direction") == 1 || command["direction"] == -1, "Invalid step direction");
        }
        if (type == "interval")
            bounded("milliseconds", 1000, 10000);
        if (type == "deviceTimer") {
            Require(command.at("enabled").is_boolean(), "Invalid timer state");
            bounded("seconds", 0.1, 99999.9);
        }
        if (type == "trigger") {
            ValidateChannels(command);
            bounded("delay", 0, 3600);
            bounded("voltageStep", 0, 30);
            bounded("currentStep", 0, 6);
            bounded("voltage", 0, 30);
            bounded("current", 0, 6);
            for (auto c : command["channels"])
                if (c == 3)
                    Require(command["voltage"] <= 5 && command["current"] <= 3 && command["voltageStep"] <= 5 &&
                                command["currentStep"] <= 3,
                            "CH3 trigger outside range");
        }
        if (type == "sequence") {
            ValidateChannels(command);
            Require(command.at("steps").is_array() && !command["steps"].empty() && command["steps"].size() <= 1000,
                    "Invalid sequence");
            const auto mode = State().value("mode", std::string("independent"));
            bool one = std::find(command["channels"].begin(), command["channels"].end(), Json(1)) !=
                       command["channels"].end(),
                 two = std::find(command["channels"].begin(), command["channels"].end(), Json(2)) !=
                       command["channels"].end();
            if (mode != "independent")
                Require(one == two, "Select the full sequence channel group");
            for (const auto &step : command["steps"]) {
                Require(step.at("durationMs").is_number_integer() && step["durationMs"] >= 100 &&
                            step["durationMs"] <= 604800000 && step.at("enabled").is_boolean(),
                        "Invalid sequence timing/output");
                for (auto c : command["channels"]) {
                    double v = step.at("voltage"), a = step.at("current");
                    Require(std::isfinite(v) && std::isfinite(a) && v >= 0 && a >= 0 &&
                                v <= (c == 3             ? 5
                                      : mode == "series" ? 60
                                                         : 30) &&
                                a <= (c == 3               ? 3
                                      : mode == "parallel" ? 12
                                                           : 6),
                            "Sequence parameter outside range");
                }
            }
        }
        if (type == "schedule") {
            ValidateChannels(command);
            Require(command.at("startMs").is_number_integer() && command["startMs"] >= 0 &&
                        command["startMs"] <= 604800000,
                    "Invalid start time");
            Require(command.at("durationMs").is_number_integer() && command["durationMs"] >= 100 &&
                        command["durationMs"] <= 604800000,
                    "Invalid duration");
        }
        if (type == "settings" || type == "diagnostic") {
            const auto op = command.at("operation").get<std::string>();
            const std::set<std::string> allowed = {
                "readTimer",    "readMode", "readProtection", "protection", "clearProtection",
                "readPanel",    "panel",    "beep",           "clearText",  "savePreset",
                "recallPreset", "identity", "errors",         "status",     "selfTest",
                "reset",        "scpi"};
            Require(allowed.count(op) != 0, "Unknown device operation");
            if (op == "readProtection" || op == "protection" || op == "clearProtection")
                ValidateChannels(command);
            if (op == "protection") {
                Require(command.at("enabled").is_boolean(), "Invalid protection state");
                bounded("limit", 0, 30);
                bounded("ovp", 0.01, 33);
                bounded("softwareVoltage", 0, 30);
                bounded("softwareCurrent", 0, 6);
                for (auto c : command["channels"])
                    if (c == 3)
                        Require(command["limit"] <= 5 && command["ovp"] <= 5.5 && command["softwareVoltage"] <= 5 &&
                                    command["softwareCurrent"] <= 3,
                                "CH3 protection outside range");
            }
            if (op == "savePreset" || op == "recallPreset")
                Require(command.at("slot").is_number_integer() && command["slot"] >= 1 && command["slot"] <= 36,
                        "Storage slot must be 1..36");
            if (op == "panel") {
                Require(command.at("display").is_boolean() && command.at("locked").is_boolean(), "Invalid panel state");
                const auto text = command.at("text").get<std::string>();
                Require(text.size() <= 32 &&
                            std::all_of(text.begin(), text.end(),
                                        [](unsigned char c) { return c >= 32 && c < 127 && c != '"' && c != ';'; }),
                        "Screen text must be at most 32 plain ASCII characters");
                Require(command.at("control") == "local" || command["control"] == "remote", "Invalid control mode");
            }
            if (op == "scpi") {
                auto line = command.at("line").get<std::string>();
                Require(!line.empty() && line.size() <= 256 && line.find_first_of("\r\n;") == std::string::npos &&
                            std::all_of(line.begin(), line.end(), [](unsigned char c) { return c >= 32 && c < 127; }),
                        "Enter one ASCII SCPI instruction");
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        Require(!stopping_ && queue_.size() < 128, "Power command queue full or shutting down");
        if (command.contains("requestId")) {
            Require(command["requestId"].is_string() && command["requestId"].get<std::string>().size() <= 128,
                    "Invalid request ID");
            for (const auto &action : actions_)
                if (action.second.value("requestId", std::string()) == command["requestId"].get<std::string>()) {
                    Require(action.second["command"] == command, "Request ID reused with different parameters");
                    return action.second;
                }
        }
        std::string id = "power-action-" + std::to_string(next_++);
        Json action = {{"id", id},
                       {"state", "queued"},
                       {"command", command},
                       {"requestId", command.value("requestId", std::string())}};
        actions_[id] = action;
        const bool priority =
            type == "stop" || type == "disconnect" || (type == "output" && !command["enabled"].get<bool>());
        if (priority) {
            for (auto it = queue_.begin(); it != queue_.end();) {
                const auto t = it->second.value("type", std::string());
                if (t == "task" || t == "sequence" || t == "schedule" ||
                    (t == "output" && it->second.value("enabled", false))) {
                    actions_[it->first]["state"] = "canceled";
                    it = queue_.erase(it);
                } else
                    ++it;
            }
            queue_.emplace_front(id, command);
        } else
            queue_.emplace_back(id, command);
        while (actions_.size() > 256) {
            auto it = std::find_if(actions_.begin(), actions_.end(), [](const auto &item) {
                return item.second["state"] != "queued" && item.second["state"] != "running";
            });
            if (it == actions_.end())
                break;
            actions_.erase(it);
        }
        wake_.notify_one();
        return action;
    } catch (const std::exception &e) {
        return {{"error", e.what()}};
    }
}
namespace ws { constexpr size_t MaxMessage=128*1024; }
Json Failure(const char *code,const std::string &detail){return {{"error",{{"code",code},{"message",detail}}}};}
struct ApiServer {
struct History {std::deque<Json> events; std::uint64_t seq=0; size_t bytes=0;};
std::map<std::string,History> histories_; std::mutex mutex_;
Json EventsLocked(const std::string&,std::uint64_t); Json Events(const std::string&,std::uint64_t);
};
Json ApiServer::EventsLocked(const std::string &target, std::uint64_t after) {
    auto it = histories_.find(target);
    if (it == histories_.end())
        return {{"events", Json::array()}, {"cursor", 0}, {"gap", after != 0}};
    auto &h = it->second;
    bool gap =
        after > h.seq || (h.events.empty() ? after < h.seq : after + 1 < h.events.front()["seq"].get<std::uint64_t>());
    Json events = Json::array();
    if (!gap)
        for (auto &e : h.events)
            if (e["seq"].get<std::uint64_t>() > after)
                events.push_back(e);
    // History retrieval is bounded by the message limit; subscription streams individual frames.
    return {{"events", events}, {"cursor", h.seq}, {"gap", gap}};
}
Json ApiServer::Events(const std::string &target, std::uint64_t after) {
    std::lock_guard<std::mutex> l(mutex_);
    auto result = EventsLocked(target, after);
    if (result.dump().size() > ws::MaxMessage)
        return Failure("HISTORY_TOO_LARGE", "Use subscribe for bounded streamed history");
    return result;
}
int main(){
Json results=Json::object();
PowerService power;
for(int i=0;i<128;++i){auto r=power.Submit({{"type","output"},{"channels",{1}},{"enabled",true}}); if(r.contains("error")) return 1;}
results["power_queue_depth_before"]=power.queue_.size();
results["power_off_at_capacity"]=power.Submit({{"type","output"},{"channels",{1}},{"enabled",false}});
results["power_stop_at_capacity"]=power.Submit({{"type","stop"}});
results["power_disconnect_at_capacity"]=power.Submit({{"type","disconnect"}});
results["power_queue_depth_after"]=power.queue_.size();
ApiServer api; auto &h=api.histories_["session-3"];
// Same event schema as Publish(), fixed valid Base64 payloads, exact JSON byte accounting.
for(int i=0;i<8;++i){ ++h.seq; h.events.push_back({{"type","event"},{"version",1},{"instance",std::string(32,'a')},{"resource","session-3"},{"seq",h.seq},{"kind","output"},{"source","device"},{"data",std::string(21000,'A')},{"tick",1000}}); }
// Tune only the final event's Base64 payload; all message metadata remains production-shaped.
h.events.pop_back(); --h.seq;
while(api.EventsLocked("session-3",0).dump().size()>130800){h.events.pop_back();--h.seq;}
++h.seq; h.events.push_back({{"type","event"},{"version",1},{"instance",std::string(32,'a')},{"resource","session-3"},{"seq",h.seq},{"kind","output"},{"source","device"},{"data",""},{"tick",1000}});
size_t room=ws::MaxMessage-api.EventsLocked("session-3",0).dump().size(); room=(room/4)*4;
h.events.back()["data"]=std::string(room,'A');
auto result=api.Events("session-3",0);
Json reply={{"type","response"},{"version",1},{"instance",std::string(32,'a')},{"requestId",std::string(128,'r')},{"ok",true},{"result",result}};
results["events_result_bytes"]=result.dump().size(); results["events_wire_payload_bytes"]=reply.dump().size(); results["events_max_message"]=ws::MaxMessage; results["events_guard_returned_error"]=result.contains("error");
std::cout<<results.dump(2)<<"\n";
return results["power_off_at_capacity"].contains("error") && !result.contains("error") && reply.dump().size()>ws::MaxMessage ? 0 : 2;
}
