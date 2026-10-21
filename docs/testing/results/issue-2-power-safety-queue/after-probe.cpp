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
        Require(!stopping_, "Power service is shutting down");
        if (command.contains("requestId")) {
            Require(command["requestId"].is_string() && command["requestId"].get<std::string>().size() <= 128,
                    "Invalid request ID");
            for (const auto &action : actions_)
                if (action.second.value("requestId", std::string()) == command["requestId"].get<std::string>()) {
                    Require(action.second["command"] == command, "Request ID reused with different parameters");
                    return action.second;
                }
        }
        // Validated safety commands have nine possible scopes: stop,
        // disconnect, and the seven nonempty OFF channel sets. Keep at most
        // one pending command per scope, reserving bounded capacity beyond the
        // 128 ordinary slots even when none of that backlog can be canceled.
        const auto safetyScope = [](const Json &item) {
            const auto t = item.at("type").get<std::string>();
            if (t == "stop") return 1;
            if (t == "disconnect") return 2;
            if (t != "output" || item.at("enabled").get<bool>()) return 0;
            int mask = 0;
            for (const auto &channel : item.at("channels"))
                mask |= 1 << (channel.get<int>() - 1);
            return 2 + mask;
        };
        const int scope = safetyScope(command);
        const bool priority = scope != 0;
        Require(priority || queue_.size() < 128, "Power command queue full");
        std::string id = "power-action-" + std::to_string(next_++);
        Json action = {{"id", id},
                       {"state", "queued"},
                       {"command", command},
                       {"requestId", command.value("requestId", std::string())}};
        actions_[id] = action;
        if (priority) {
            for (auto it = queue_.begin(); it != queue_.end();) {
                const auto t = it->second.value("type", std::string());
                if (t == "task" || t == "sequence" || t == "schedule" ||
                    (t == "output" && it->second.value("enabled", false)) || safetyScope(it->second) == scope) {
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
int main() {
int failures=0; Json results=Json::array();
auto check=[&](bool ok,const char* name){results.push_back({{"test",name},{"passed",ok}});if(!ok)++failures;};
for (auto safety : std::vector<Json>{ {{"type","output"},{"channels",{1}},{"enabled",false},{"source","local"}}, {{"type","output"},{"channels",{1}},{"enabled",false},{"source","network"}}, {{"type","stop"}}, {{"type","disconnect"}} }) {
 PowerService p; std::vector<std::string> pending;
 for(int i=0;i<128;++i) { auto a=p.Submit({{"type","output"},{"channels",{2}},{"enabled",true},{"requestId",std::to_string(i)}}); if(!a.contains("error"))pending.push_back(a["id"]); }
 check(pending.size()==128,"128 ON queued");
 auto bad=p.Submit({{"type","output"},{"channels",{1,1}},{"enabled",false}});
 check(bad.contains("error") && p.queue_.size()==128,"invalid safety has no side effects");
 auto conflict=p.Submit({{"type","output"},{"channels",{2}},{"enabled",false},{"requestId","0"}});
 check(conflict.contains("error") && p.queue_.size()==128,"conflicting request ID has no side effects");
 auto a=p.Submit(safety); check(!a.contains("error") && a.value("state","")=="queued","safety accepted at capacity, not claimed completed");
 check(p.queue_.size()==1,"stale ON backlog removed");
 check(std::all_of(pending.begin(),pending.end(),[&](auto& id){return p.actions_[id]["state"]=="canceled";}),"all pending ON canceled");
}
PowerService p;
Json first={{"type","parameters"},{"channels",{1}},{"voltage",1},{"current",1},{"requestId","ordinary-0"}};
auto original=p.Submit(first);
for(int i=1;i<128;++i) { auto q=first;q["requestId"]="ordinary-"+std::to_string(i);p.Submit(q); }
check(p.queue_.size()==128,"128 noncancelable commands queued");
check(p.Submit(first).value("id","")==original.value("id","missing"),"dedup at full queue");
auto overflow=first;overflow["requestId"]="overflow";
check(p.Submit(overflow).contains("error"),"ordinary overflow rejected");
bool accepted=true,bounded=true,canceled=true;
std::map<int,std::string> previous;
for(int repeat=0;repeat<100;++repeat) {
 for(int mask=1;mask<8;++mask) {
  Json channels=Json::array();for(int c=1;c<=3;++c)if(mask&(1<<(c-1)))channels.push_back(c);
  if(repeat%2)std::reverse(channels.begin(),channels.end());
  auto a=p.Submit({{"type","output"},{"channels",channels},{"enabled",false},{"source",repeat%2?"network":"local"}});
  accepted=accepted&&!a.contains("error");bounded=bounded&&p.queue_.size()<=137;
  if(!a.contains("error")) {if(previous.count(mask)){auto old=p.actions_.find(previous[mask]);if(old!=p.actions_.end())canceled=canceled&&old->second["state"]=="canceled";}previous[mask]=a["id"];}
 }
 for(auto type:{"stop","disconnect"})accepted=accepted&&!p.Submit({{"type",type}}).contains("error");
 bounded=bounded&&p.queue_.size()<=137&&p.actions_.size()<=256;
}
check(accepted,"900 safety requests accepted over noncancelable backlog");
check(bounded && p.queue_.size()==137,"queue <=137 and action history <=256 under repeated safety requests");
check(canceled,"retained superseded OFF reported canceled");
check(p.actions_[original["id"]]["state"]=="queued","unrelated ordinary work retained");
p.stopping_=true;auto q=p.queue_.size();
check(p.Submit({{"type","stop"}}).contains("error")&&p.queue_.size()==q,"shutdown rejects safety without queue mutation");
std::cout<<Json{{"failures",failures},{"results",results}}.dump(2)<<std::endl;return failures?1:0;
}
