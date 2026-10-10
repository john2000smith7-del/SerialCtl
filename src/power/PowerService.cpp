#include "PowerService.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <locale>
#include <set>
#include <shlobj.h>
#include <sstream>
#include <stdexcept>
namespace serialctl {
namespace {
using Vi = unsigned long;
using Vs = long;
struct Visa {
    HMODULE dll = nullptr;
    Vs(WINAPI *openRM)(Vi *) = nullptr;
    Vs(WINAPI *find)(Vi, const char *, Vi *, Vi *, char *) = nullptr;
    Vs(WINAPI *next)(Vi, char *) = nullptr;
    Vs(WINAPI *open)(Vi, const char *, Vi, Vi, Vi *) = nullptr;
    Vs(WINAPI *close)(Vi) = nullptr;
    Vs(WINAPI *attr)(Vi, Vi, std::uint64_t) = nullptr;
    Vs(WINAPI *write)(Vi, const unsigned char *, Vi, Vi *) = nullptr;
    Vs(WINAPI *read)(Vi, unsigned char *, Vi, Vi *) = nullptr;
    Visa() {
        wchar_t sys[MAX_PATH]{};
        GetSystemDirectoryW(sys, MAX_PATH);
        dll = LoadLibraryW((std::wstring(sys) + L"\\visa64.dll").c_str());
        if (!dll)
            return;
#define VISA_PROC(member, name) member = reinterpret_cast<decltype(member)>(GetProcAddress(dll, name))
        VISA_PROC(openRM, "viOpenDefaultRM");
        VISA_PROC(find, "viFindRsrc");
        VISA_PROC(next, "viFindNext");
        VISA_PROC(open, "viOpen");
        VISA_PROC(close, "viClose");
        VISA_PROC(attr, "viSetAttribute");
        VISA_PROC(write, "viWrite");
        VISA_PROC(read, "viRead");
#undef VISA_PROC
        if (!openRM || !find || !next || !open || !close || !attr || !write || !read) {
            FreeLibrary(dll);
            dll = nullptr;
        }
    }
    ~Visa() {
        if (dll)
            FreeLibrary(dll);
    }
};
void Require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string Decimal(double value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(12);
    text << value;
    return text.str();
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
std::string Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char text[40]{};
    sprintf_s(text, "%04u-%02u-%02u %02u:%02u:%02u.%03u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
              now.wSecond, now.wMilliseconds);
    return text;
}
} // namespace
struct PowerService::Transport {
    Visa visa;
    Vi rm = 0, device = 0;
    HANDLE serial = INVALID_HANDLE_VALUE;
    bool simulated = false;
    int failAfter = -1;
    Json simulation =
        Json::array({Json{{"voltage", 0.0}, {"current", 0.0}, {"output", false}, {"ovp", 30.0}, {"ovpEnabled", false}},
                     Json{{"voltage", 0.0}, {"current", 0.0}, {"output", false}, {"ovp", 30.0}, {"ovpEnabled", false}},
                     Json{{"voltage", 0.0}, {"current", 0.0}, {"output", false}, {"ovp", 5.5}, {"ovpEnabled", false}}});
    int selected = 0;
    std::string mode = "independent", screen;
    bool display = true, timer = false;
    double delay = 5, triggerDelay = 0;
    std::map<int, Json> presets;
    Json writes = Json::array();
    ~Transport() {
        if (device)
            visa.close(device);
        if (rm)
            visa.close(rm);
        if (serial != INVALID_HANDLE_VALUE)
            CloseHandle(serial);
    }
    void Write(std::string command) {
        if (simulated) {
            writes.push_back(command);
            if (command == "OUTP:PAR ON")
                mode = "parallel";
            else if (command == "OUTP:SER ON")
                mode = "series";
            else if (command == "OUTP:TRAC ON")
                mode = "tracking";
            else if (command == "OUTP:PAR OFF" || command == "OUTP:SER OFF" || command == "OUTP:TRAC OFF")
                mode = "independent";
            else if (command.rfind("*SAV ", 0) == 0)
                presets[std::stoi(command.substr(5))] = simulation;
            else if (command.rfind("*RCL ", 0) == 0) {
                auto i = presets.find(std::stoi(command.substr(5)));
                if (i != presets.end())
                    simulation = i->second;
            } else if (command == "DISP ON" || command == "DISP OFF")
                display = command == "DISP ON";
            else if (command.rfind("DISP:TEXT ", 0) == 0)
                screen = command.substr(10);
            else if (command == "DISP:TEXT:CLEAR")
                screen.clear();
            else if (command == "OUTP:TIM ON" || command == "OUTP:TIM OFF")
                timer = command == "OUTP:TIM ON";
            else if (command.rfind("OUTP:TIM:DEL ", 0) == 0)
                delay = std::stod(command.substr(13));
            else if (command.rfind("TRIG:DEL ", 0) == 0)
                triggerDelay = std::stod(command.substr(9));
            else if (command == "*RST") {
                mode = "independent";
                for (auto &c : simulation) {
                    c["voltage"] = 0.0;
                    c["current"] = 0.0;
                    c["output"] = false;
                }
            } else if (command.rfind("VOLT:LIMIT ", 0) == 0)
                simulation[selected]["limit"] = std::stod(command.substr(11));
            else if (command.rfind("VOLT:STEP ", 0) == 0)
                simulation[selected]["vstep"] = std::stod(command.substr(10));
            else if (command.rfind("CURR:STEP ", 0) == 0)
                simulation[selected]["astep"] = std::stod(command.substr(10));
            else if (command.rfind("VOLT:TRIG ", 0) == 0)
                simulation[selected]["vtrig"] = std::stod(command.substr(10));
            else if (command.rfind("CURR:TRIG ", 0) == 0)
                simulation[selected]["atrig"] = std::stod(command.substr(10));
            else if (command == "*TRG")
                for (auto &c : simulation) {
                    if (c.contains("vtrig"))
                        c["voltage"] = c["vtrig"];
                    if (c.contains("atrig"))
                        c["current"] = c["atrig"];
                }
            else if (command.rfind("INST:NSEL ", 0) == 0)
                selected = std::stoi(command.substr(10)) - 1;
            else if (command == "VOLT:UP" || command == "VOLT:DOWN")
                simulation[selected]["voltage"] =
                    simulation[selected]["voltage"].get<double>() +
                    (command == "VOLT:UP" ? 1 : -1) * simulation[selected].value("vstep", 0.1);
            else if (command == "CURR:UP" || command == "CURR:DOWN")
                simulation[selected]["current"] =
                    simulation[selected]["current"].get<double>() +
                    (command == "CURR:UP" ? 1 : -1) * simulation[selected].value("astep", 0.1);
            else if (command.rfind("VOLT ", 0) == 0)
                simulation[selected]["voltage"] = std::stod(command.substr(5));
            else if (command.rfind("CURR ", 0) == 0)
                simulation[selected]["current"] = std::stod(command.substr(5));
            else if (command.rfind("VOLT:PROT:STAT ", 0) == 0)
                simulation[selected]["ovpEnabled"] = command.substr(15) == "ON";
            else if (command.rfind("VOLT:PROT ", 0) == 0)
                simulation[selected]["ovp"] = std::stod(command.substr(10));
            else if (command.rfind("CHAN:OUTP ", 0) == 0)
                simulation[selected]["output"] = command.substr(10) == "ON";
            return;
        }
        command += "\n";
        DWORD count = 0;
        if (device) {
            Vi written = 0;
            Require(visa.write(device, reinterpret_cast<const unsigned char *>(command.data()),
                               static_cast<Vi>(command.size()), &written) >= 0 &&
                        written == command.size(),
                    "USB write failed");
        } else
            Require(WriteFile(serial, command.data(), static_cast<DWORD>(command.size()), &count, nullptr) &&
                        count == command.size(),
                    "Serial write failed");
    }
    std::string Query(std::string command) {
        if (simulated) {
            if (failAfter == 0)
                throw std::runtime_error("Simulated communication timeout");
            if (failAfter > 0)
                --failAfter;
            if (command == "OUTP:PAR?")
                return mode == "parallel" ? "1" : "0";
            if (command == "OUTP:SER?")
                return mode == "series" ? "1" : "0";
            if (command == "OUTP:TRAC?")
                return mode == "tracking" ? "1" : "0";
            if (command == "SYST:ERR?")
                return "0,\"No error\"";
            if (command == "SYST:VERS?")
                return "1999.0";
            if (command == "DISP?")
                return display ? "1" : "0";
            if (command == "DISP:TEXT?")
                return screen;
            if (command == "OUTP:TIM?")
                return timer ? "1" : "0";
            if (command == "OUTP:TIM:DEL?")
                return Decimal(delay);
            if (command == "TRIG:DEL?")
                return Decimal(triggerDelay);
            if (command == "VOLT:LIMIT?")
                return Decimal(simulation[selected].value("limit", selected == 2 ? 5.0 : 30.0));
            if (command == "VOLT:STEP?")
                return Decimal(simulation[selected].value("vstep", 0.1));
            if (command == "CURR:STEP?")
                return Decimal(simulation[selected].value("astep", 0.1));
            if (command == "INST:NSEL?")
                return std::to_string(selected + 1);
            if (command == "*IDN?")
                return "ITECH,IT6332A,SIMULATION,1.0";
            if (command == "CHAN:OUTP?")
                return simulation[selected]["output"].get<bool>() ? "1" : "0";
            const bool enabled = simulation[selected]["output"].get<bool>();
            if (command == "VOLT:PROT? MAX")
                return selected == 2 ? "5.5" : "33";
            if (command == "VOLT:PROT?")
                return Decimal(simulation[selected]["ovp"].get<double>());
            if (command == "VOLT:PROT:STAT?")
                return simulation[selected]["ovpEnabled"].get<bool>() ? "1" : "0";
            if (command == "VOLT:PROT:TRIP?")
                return "0";
            if (command == "VOLT?")
                return Decimal(simulation[selected]["voltage"].get<double>());
            if (command == "CURR?")
                return Decimal(simulation[selected]["current"].get<double>());
            if (command == "MEAS:VOLT?")
                return enabled ? Decimal(simulation[selected]["voltage"].get<double>()) : "0";
            if (command == "MEAS:CURR?")
                return "0";
            return "0";
        }
        Write(command);
        std::string result;
        auto end = GetTickCount64() + 2500;
        while (GetTickCount64() < end && result.size() < 4096) {
            unsigned char buffer[256]{};
            DWORD count = 0;
            bool endOfTransfer = false;
            if (device) {
                Vi got = 0;
                Vs status = visa.read(device, buffer, sizeof(buffer), &got);
                Require(status >= 0, "USB query timeout or read failure");
                count = got;
                endOfTransfer = status != 0x3FFF0006 && count > 0;
            } else
                Require(ReadFile(serial, buffer, sizeof(buffer), &count, nullptr) != FALSE, "Serial read failed");
            result.append(reinterpret_cast<char *>(buffer), count);
            if (result.find('\n') != std::string::npos || (device && endOfTransfer)) {
                while (!result.empty() && (result.back() == '\r' || result.back() == '\n'))
                    result.pop_back();
                return result;
            }
        }
        throw std::runtime_error("SCPI query timed out");
    }
};
bool PowerService::VisaAvailable() {
    Visa visa;
    return visa.dll != nullptr;
}
Json PowerService::UsbResources() {
    Visa visa;
    Json resources = Json::array();
    if (!visa.dll)
        return resources;
    Vi rm = 0, list = 0, count = 0;
    char name[512]{};
    if (visa.openRM(&rm) < 0)
        return resources;
    if (visa.find(rm, "USB?*INSTR", &list, &count, name) >= 0) {
        for (Vi i = 0; i < std::min<Vi>(count, 64); ++i) {
            if (i && visa.next(list, name) < 0)
                break;
            resources.push_back(name);
        }
        visa.close(list);
    }
    visa.close(rm);
    return resources;
}
PowerService::PowerService() {
    state_ = {{"id", "power-1"},           {"connected", false},          {"model", "IT6332A"},
              {"simulation", false},       {"mode", "independent"},       {"records", Json::array()},
              {"channels", Json::array()}, {"task", {{"running", false}}}};
    for (int i = 1; i <= 3; ++i)
        state_["channels"].push_back({{"channel", i},
                                      {"maxVoltage", i == 3 ? 5 : 30},
                                      {"maxCurrent", i == 3 ? 3 : 6},
                                      {"setVoltage", nullptr},
                                      {"setCurrent", nullptr},
                                      {"voltage", nullptr},
                                      {"current", nullptr},
                                      {"power", nullptr},
                                      {"output", nullptr},
                                      {"timestamp", nullptr}});
    thread_ = std::thread(&PowerService::Run, this);
}
PowerService::~PowerService() {
    Shutdown();
}
Json PowerService::State() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
Json PowerService::Action(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = actions_.find(id);
    return it == actions_.end() ? Json{{"error", "Action expired or not found"}} : it->second;
}
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
void PowerService::Command(const std::string &command) {
    Require(transport_ != nullptr, "Power supply disconnected");
    transport_->Write(command);
}
std::string PowerService::Query(const std::string &command) {
    Require(transport_ != nullptr, "Power supply disconnected");
    return transport_->Query(command);
}
double PowerService::Number(const std::string &command) {
    std::string value = Query(command);
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    double number = 0;
    input >> number;
    Require(!input.fail() && std::isfinite(number), "Invalid measurement response");
    input >> std::ws;
    Require(input.eof(), "Unexpected SCPI response");
    return number;
}
void PowerService::Select(int channel) {
    Command("INST:NSEL " + std::to_string(channel));
    Require(Number("INST:NSEL?") == channel, "Channel selection readback differs");
}
void PowerService::Outputs(const Json &channels, bool enabled, Json &result) {
    if (enabled)
        ReadMode();
    auto mode = State().value("mode", std::string("independent"));
    if (mode == "series" || mode == "parallel" || mode == "tracking") {
        bool one = std::find(channels.begin(), channels.end(), Json(1)) != channels.end();
        bool two = std::find(channels.begin(), channels.end(), Json(2)) != channels.end();
        Require(one == two, "Select both CH1 and CH2 in combined mode");
    }
    result = Json::array();
    bool failed = false;
    for (const auto &c : channels) {
        int channel = c.get<int>();
        Json item = {{"channel", channel}, {"state", "skipped"}};
        if (!failed || !enabled)
            try {
                if (failed) {
                    Command("INST:NSEL " + std::to_string(channel));
                    Command("CHAN:OUTP OFF");
                    item["state"] = "unknown";
                    item["error"] = "OFF written without confirmation after communication failure";
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_["channels"][channel - 1]["output"] = nullptr;
                    result.push_back(item);
                    continue;
                }
                Select(channel);
                Command(enabled ? "CHAN:OUTP ON" : "CHAN:OUTP OFF");
                bool confirmed = Number("CHAN:OUTP?") != 0;
                Require(confirmed == enabled, "Output readback differs");
                item["state"] = "confirmed";
                item["output"] = confirmed;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_["channels"][channel - 1]["output"] = confirmed;
                }
            } catch (const std::exception &e) {
                failed = true;
                item["state"] = "unknown";
                item["error"] = e.what();
                if (!enabled) {
                    try {
                        Command("INST:NSEL " + std::to_string(channel));
                        Command("CHAN:OUTP OFF");
                        item["shutdownAttempt"] = "written-unconfirmed";
                    } catch (...) {
                        item["shutdownAttempt"] = "failed";
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_["channels"][channel - 1]["output"] = nullptr;
                }
            }
        result.push_back(item);
    }
    log_.WriteStatus(
        MultiByteToWide(reinterpret_cast<const std::uint8_t *>(result.dump().data()), result.dump().size(), CP_UTF8));
    if (failed && enabled) {
        for (auto &item : result) {
            int channel = item["channel"];
            try {
                Command("INST:NSEL " + std::to_string(channel));
                Command("CHAN:OUTP OFF");
                item["shutdownAttempt"] = "written-unconfirmed";
            } catch (...) {
                item["shutdownAttempt"] = "failed";
            }
            std::lock_guard<std::mutex> lock(mutex_);
            state_["channels"][channel - 1]["output"] = nullptr;
        }
    }
    if (failed)
        throw std::runtime_error("Output operation incomplete; inspect per-channel result");
}
void PowerService::StopTask() {
    if (owned_.empty())
        return;
    Json channels = owned_;
    Json completed = task_;
    task_ = Json();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed.is_object())
            state_["task"] = completed;
        state_["task"]["running"] = false;
    }
    if (transport_) {
        Json result;
        Outputs(channels, false, result);
    }
    owned_ = Json::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_["ownedChannels"] = owned_;
    }
}
void PowerService::Disconnect() {
    try {
        StopTask();
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_["error"] = e.what();
    }
    transport_.reset();
    log_.Stop();
    measurements_.Stop();
    std::lock_guard<std::mutex> lock(mutex_);
    state_["connected"] = false;
    state_["task"]["running"] = false;
    task_ = Json();
    owned_ = Json::array();
    state_["ownedChannels"] = owned_;
    for (auto &channel : state_["channels"]) {
        channel["output"] = nullptr;
        channel["voltage"] = nullptr;
        channel["current"] = nullptr;
        channel["power"] = nullptr;
        channel.erase("protection");
    }
}
void PowerService::Execute(const std::string &id, const Json &command) {
    Json action;
    bool communication = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actions_[id]["state"] = "running";
        action = actions_[id];
    }
    try {
        std::string type = command["type"];
        if (type == "connect") {
            Require(!transport_, "Already connected");
            auto transport = std::make_unique<Transport>();
            std::string backend = command.at("backend");
            if (backend == "simulation") {
                transport->simulated = true;
                transport->failAfter = command.value("testFailAfterReads", -1);
            } else if (backend == "usb") {
                Require(transport->visa.dll != nullptr, "Install bundled VISA runtime first");
                Require(transport->visa.openRM(&transport->rm) >= 0, "VISA unavailable");
                const auto resource = command.at("resource").get<std::string>();
                Require(resource.rfind("USB", 0) == 0 && resource.size() < 512, "Invalid USB resource");
                Require(transport->visa.open(transport->rm, resource.c_str(), 0, 2500, &transport->device) >= 0,
                        "Cannot open USB supply");
                Require(transport->visa.attr(transport->device, 0x3FFF001A, 2500) >= 0, "Cannot set USB timeout");
            } else if (backend == "serial") {
                std::wstring port = MultiByteToWide(
                    reinterpret_cast<const std::uint8_t *>(command.at("port").get_ref<const std::string &>().data()),
                    command.at("port").get_ref<const std::string &>().size(), CP_UTF8);
                Require(port.rfind(L"COM", 0) == 0 && port.size() > 3 && port.size() <= 12 &&
                            port.find_first_not_of(L"0123456789", 3) == std::wstring::npos,
                        "Invalid COM name");
                transport->serial = CreateFileW((L"\\\\.\\" + port).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                                OPEN_EXISTING, 0, nullptr);
                Require(transport->serial != INVALID_HANDLE_VALUE, "COM busy or unavailable");
                DCB dcb{};
                dcb.DCBlength = sizeof(dcb);
                Require(GetCommState(transport->serial, &dcb) != FALSE, "Cannot read COM settings");
                dcb.BaudRate = command.value("baud", 9600);
                dcb.ByteSize = static_cast<BYTE>(command.value("dataBits", 8));
                dcb.Parity = static_cast<BYTE>(command.value("parity", 0));
                dcb.StopBits = static_cast<BYTE>(command.value("stopBits", 0));
                dcb.fBinary = TRUE;
                dcb.fParity = dcb.Parity != NOPARITY;
                dcb.fOutxCtsFlow = FALSE;
                dcb.fOutxDsrFlow = FALSE;
                dcb.fOutX = dcb.fInX = FALSE;
                dcb.fDtrControl = DTR_CONTROL_DISABLE;
                dcb.fRtsControl = RTS_CONTROL_DISABLE;
                Require(SetCommState(transport->serial, &dcb) != FALSE, "Invalid serial configuration");
                COMMTIMEOUTS timeouts{MAXDWORD, 0, 250, 0, 2500};
                Require(SetCommTimeouts(transport->serial, &timeouts) != FALSE, "Cannot set COM timeout");
                PurgeComm(transport->serial, PURGE_RXCLEAR | PURGE_TXCLEAR);
            } else
                throw std::runtime_error("Unknown connection backend");
            std::string identity = transport->Query("*IDN?");
            auto comma = identity.find(',');
            auto nextComma = comma == std::string::npos ? std::string::npos : identity.find(',', comma + 1);
            std::string model =
                comma == std::string::npos ? std::string() : identity.substr(comma + 1, nextComma - comma - 1);
            model.erase(
                std::remove_if(model.begin(), model.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
                model.end());
            Require(model == "IT6332A", "Instrument is not IT6332A");
            transport_ = std::move(transport);
            Select(1);
            ReadMode();
            std::wstring error;
            log_.Start(L"IT6332A", error);
            std::wstring csvError;
            measurements_.Start(L"IT6332A", csvError, true);
            const std::string header = "timestamp,channel,voltage_V,current_A,power_W,output\r\n";
            measurements_.WriteRaw(Bytes(header.begin(), header.end()));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_["connected"] = true;
                state_["identity"] = identity;
                state_["backend"] = backend;
                state_["simulation"] = backend == "simulation";
                state_.erase("error");
                state_.erase("logError");
                state_["csvPath"] = WideToMultiByte(measurements_.Path(), CP_UTF8);
                if (!csvError.empty())
                    state_["logError"] = WideToMultiByte(csvError, CP_UTF8);
                state_["logPath"] = WideToMultiByte(log_.Path(), CP_UTF8);
                if (!error.empty())
                    state_["logError"] = WideToMultiByte(error, CP_UTF8);
            }
            Poll();
            pollAt_ = GetTickCount64() + pollInterval_;
        } else if (type == "disconnect")
            Disconnect();
        else if (type == "stop")
            StopTask();
        else if (type == "output") {
            if (command["enabled"].get<bool>())
                Require(task_.is_null() || task_.empty(), "Stop the running task before manual output ON");
            if (!command["enabled"].get<bool>()) {
                task_ = Json();
                std::lock_guard<std::mutex> lock(mutex_);
                state_["task"]["running"] = false;
            }
            communication = true;
            Outputs(command["channels"], command["enabled"], action["channels"]);
        } else if (type == "parameters") {
            Require(task_.is_null() || task_.empty(), "Stop the running task before changing parameters");
            communication = true;
            Json result = Json::array();
            for (const auto &c : command["channels"]) {
                int channel = c.get<int>();
                Select(channel);
                Command("VOLT " + Decimal(command["voltage"]));
                Command("CURR " + Decimal(command["current"]));
                double v = Number("VOLT?"), a = Number("CURR?");
                Require(std::abs(v - command["voltage"].get<double>()) < 0.02 &&
                            std::abs(a - command["current"].get<double>()) < 0.02,
                        "Parameter readback differs");
                result.push_back({{"channel", channel}, {"voltage", v}, {"current", a}});
            }
            action["channels"] = result;
            Poll();
        } else if (type == "protection") {
            Require(task_.is_null() || task_.empty(), "Stop the task before changing protection");
            communication = true;
            // Preflight every hardware limit before changing any selected channel.
            for (const auto &c : command["channels"]) {
                Select(c.get<int>());
                Require(command["voltageLimit"].get<double>() <= Number("VOLT:PROT? MAX"),
                        "OVP threshold exceeds instrument range");
            }
            for (const auto &c : command["channels"]) {
                int channel = c.get<int>();
                Select(channel);
                Command("VOLT:PROT " + Decimal(command["voltageLimit"]));
                Command(command["enabled"].get<bool>() ? "VOLT:PROT:STAT ON" : "VOLT:PROT:STAT OFF");
                Require(std::abs(Number("VOLT:PROT?") - command["voltageLimit"].get<double>()) < 0.02 &&
                            (Number("VOLT:PROT:STAT?") != 0) == command["enabled"].get<bool>(),
                        "Protection readback differs");
                std::lock_guard<std::mutex> lock(mutex_);
                state_["channels"][channel - 1]["protection"] = {{"enabled", command["enabled"]},
                                                                 {"voltageLimit", command["voltageLimit"]},
                                                                 {"currentLimit", command["currentLimit"]}};
            }
        } else if (type == "task" || type == "sequence" || type == "schedule") {
            Require(transport_ != nullptr, "Supply disconnected");
            Require(task_.is_null() || task_.empty(), "A task is already running");
            Require(owned_.empty(), "Stop and power off the previous canceled task first");
            task_ = command;
            owned_ = command["channels"];
            task_["completed"] = 0;
            task_["phase"] = "off";
            task_["index"] = 0;
            taskAt_ = GetTickCount64() + command.value("startMs", 0ull);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_["task"] = task_;
                state_["task"]["running"] = true;
                state_["ownedChannels"] = owned_;
            }
        } else {
            Require(task_.is_null() || task_.empty() || type == "interval", "Stop the running task first");
            communication = type != "interval";
            Extended(command, action);
        }
        action["state"] = "completed";
    } catch (const std::exception &e) {
        action["state"] = "failed";
        action["error"] = e.what();
        action["timestamp"] = Timestamp();
        auto failureRecord = action.dump();
        log_.WriteStatus(MultiByteToWide(reinterpret_cast<const std::uint8_t *>(failureRecord.data()),
                                         failureRecord.size(), CP_UTF8));
        if (command.value("type", std::string()) == "connect" || communication) {
            Disconnect();
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &job : queue_)
                actions_[job.first]["state"] = "canceled";
            queue_.clear();
        }
        action["state"] = "failed";
        action["error"] = e.what();
        std::lock_guard<std::mutex> lock(mutex_);
        state_["error"] = e.what();
    }
    action["timestamp"] = Timestamp();
    log_.WriteStatus(
        MultiByteToWide(reinterpret_cast<const std::uint8_t *>(action.dump().data()), action.dump().size(), CP_UTF8));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actions_[id] = action;
        state_["records"].push_back({{"timestamp", action["timestamp"]},
                                     {"source", command.value("source", std::string("local"))},
                                     {"operation", command["type"]},
                                     {"result", action.value("error", action.value("state", std::string()))}});
        if (state_["records"].size() > 200)
            state_["records"].erase(state_["records"].begin());
        if (transport_ && transport_->simulated)
            state_["testWrites"] = transport_->writes;
    }
}
void PowerService::Poll() {
    ReadMode();
    Json channels = State()["channels"];
    for (int i = 0; i < 3; ++i) {
        Select(i + 1);
        auto &c = channels[i];
        c["setVoltage"] = Number("VOLT?");
        c["setCurrent"] = Number("CURR?");
        c["output"] = Number("CHAN:OUTP?") != 0;
        c["voltage"] = Number("MEAS:VOLT?");
        c["current"] = Number("MEAS:CURR?");
        c["power"] = c["voltage"].get<double>() * c["current"].get<double>();
        c["timestamp"] = Timestamp();
        std::string csv = c["timestamp"].get<std::string>() + "," + std::to_string(i + 1) + "," +
                          Decimal(c["voltage"]) + "," + Decimal(c["current"]) + "," + Decimal(c["power"]) + "," +
                          (c["output"].get<bool>() ? "ON" : "OFF") + "\r\n";
        measurements_.WriteRaw(Bytes(csv.begin(), csv.end()));
        c["ovpTripped"] = Number("VOLT:PROT:TRIP?") != 0;
        if (c["ovpTripped"].get<bool>() || c["output"].get<bool>()) {
            bool fault = c["ovpTripped"].get<bool>();
            if (c.contains("protection") && c["protection"].value("enabled", false)) {
                fault = fault || c["voltage"].get<double>() > c["protection"]["voltageLimit"].get<double>() ||
                        c["current"].get<double>() > c["protection"]["currentLimit"].get<double>();
            }
            if (fault) {
                log_.WriteStatus(L"通道 " + std::to_wstring(i + 1) + L" 保护触发，停止测试并请求掉电");
                StopTask();
                task_ = Json();
                Json off;
                auto mode = State().value("mode", std::string("independent"));
                Outputs(i < 2 && mode != "independent" ? Json::array({1, 2}) : Json::array({i + 1}), false, off);
                c["output"] = false;
                std::lock_guard<std::mutex> lock(mutex_);
                state_["task"]["running"] = false;
                state_["error"] = "Channel protection triggered; output disabled";
                state_["channels"][i] = c;
                return;
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_["channels"] = channels;
    if (!measurements_.Error().empty())
        state_["logError"] = WideToMultiByte(measurements_.Error(), CP_UTF8);
    if (!log_.Error().empty())
        state_["logError"] = WideToMultiByte(log_.Error(), CP_UTF8);
}
void PowerService::Run() {
    for (;;) {
        std::pair<std::string, Json> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(50), [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
                break;
            if (!queue_.empty()) {
                job = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (!job.first.empty())
            Execute(job.first, job.second);
        if (!transport_)
            continue;
        try {
            auto now = GetTickCount64();
            if (!task_.is_null() && !task_.empty() && now >= taskAt_)
                AdvanceTask();
            if (now >= pollAt_) {
                Poll();
                pollAt_ = GetTickCount64() + pollInterval_;
            }
        } catch (const std::exception &e) {
            std::string error = e.what();
            log_.WriteStatus(
                MultiByteToWide(reinterpret_cast<const std::uint8_t *>(error.data()), error.size(), CP_UTF8));
            Disconnect();
            std::lock_guard<std::mutex> lock(mutex_);
            state_["error"] = error;
        }
    }
    Disconnect();
}
void PowerService::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
        wake_.notify_all();
    }
    if (thread_.joinable())
        thread_.join();
}
bool PowerService::ExportLog(bool csv, const std::wstring &path, std::wstring &error) {
    return csv ? measurements_.SaveCopy(path, error) : log_.SaveCopy(path, error);
}
void PowerService::CheckScpi() {
    auto error = Query("SYST:ERR?");
    std::istringstream in(error);
    in.imbue(std::locale::classic());
    int code = 0;
    in >> code;
    Require(!in.fail(), "Invalid device error response");
    if (code)
        throw std::runtime_error("Device rejected command: " + error);
}
void PowerService::ReadMode() {
    const bool track = Number("OUTP:TRAC?") != 0, series = Number("OUTP:SER?") != 0,
               parallel = Number("OUTP:PAR?") != 0;
    Require(static_cast<int>(track) + series + parallel <= 1, "Conflicting device channel combination");
    std::lock_guard<std::mutex> lock(mutex_);
    state_["mode"] = parallel ? "parallel" : series ? "series" : track ? "tracking" : "independent";
}
void PowerService::ChangeMode(const std::string &mode) {
    StopTask();
    Json off;
    Outputs(Json::array({1, 2, 3}), false, off);
    // Release every existing combination before applying the next one (manual p.41).
    Command("INST:COM:SER NONE");
    Command("INST:COM:PARA NONE");
    Command("INST:COM:TRAC NONE");
    Command("OUTP:SER OFF");
    Command("OUTP:PAR OFF");
    Command("OUTP:TRAC OFF");
    CheckScpi();
    if (mode != "independent")
        Command(mode == "parallel" ? "OUTP:PAR ON" : mode == "series" ? "OUTP:SER ON" : "OUTP:TRAC ON");
    CheckScpi();
    ReadMode();
    Require(State()["mode"] == mode, "Mode readback differs");
    Outputs(Json::array({1, 2, 3}), false, off);
    Poll();
}
void PowerService::SetParameters(const Json &settings) {
    const auto mode = State().value("mode", std::string("independent"));
    // Validate the complete batch against the current instrument limits before any write.
    Json expanded = Json::array();
    for (const auto &item : settings) {
        const auto channels = ValidateChannels(item);
        bool group = channels == Json::array({1, 2}) && mode != "independent";
        if (mode != "independent" && (std::find(channels.begin(), channels.end(), Json(1)) != channels.end() ||
                                      std::find(channels.begin(), channels.end(), Json(2)) != channels.end()))
            Require(group, "Select the complete channel group");
        double v = item.at("voltage"), a = item.at("current");
        if (group && mode == "series")
            v /= 2;
        if (group && mode == "parallel")
            a /= 2;
        for (auto ch : channels) {
            int n = ch;
            Require(v >= 0 && a >= 0 && std::isfinite(v) && std::isfinite(a) && v <= (n == 3 ? 5 : 30) &&
                        a <= (n == 3 ? 3 : 6),
                    "Parameter outside physical channel range");
            Select(n);
            Require(v <= Number("VOLT:LIMIT?"), "Voltage exceeds device setting limit");
            expanded.push_back({{"channel", n}, {"voltage", v}, {"current", a}});
        }
    }
    for (const auto &item : expanded) {
        Select(item["channel"]);
        Command("VOLT " + Decimal(item["voltage"]));
        Command("CURR " + Decimal(item["current"]));
        CheckScpi();
    }
    for (const auto &item : expanded) {
        Select(item["channel"]);
        Require(std::abs(Number("VOLT?") - item["voltage"].get<double>()) < 0.02 &&
                    std::abs(Number("CURR?") - item["current"].get<double>()) < 0.02,
                "Parameter readback differs");
    }
    Poll();
}
void PowerService::Extended(const Json &command, Json &action) {
    const auto type = command.at("type").get<std::string>();
    if (type == "interval") {
        pollInterval_ = command["milliseconds"];
        std::lock_guard<std::mutex> lock(mutex_);
        state_["sampleMs"] = pollInterval_;
        return;
    }
    Require(transport_ != nullptr, "Supply disconnected");
    if (type == "mode") {
        ChangeMode(command["mode"]);
        return;
    }
    if (type == "batch") {
        Require(command.value("mode", std::string("independent")) == State()["mode"],
                "Device mode changed; read it before applying settings");
        SetParameters(command["settings"]);
        return;
    }
    if (type == "deviceTimer") {
        Command("OUTP:TIM:DEL " + Decimal(command["seconds"]));
        Command(command["enabled"].get<bool>() ? "OUTP:TIM ON" : "OUTP:TIM OFF");
        CheckScpi();
        action["result"] = {{"enabled", Number("OUTP:TIM?") != 0}, {"seconds", Number("OUTP:TIM:DEL?")}};
        Require(action["result"]["enabled"] == command["enabled"] &&
                    std::abs(action["result"]["seconds"].get<double>() - command["seconds"].get<double>()) < 0.02,
                "Timer readback differs");
        std::lock_guard<std::mutex> lock(mutex_);
        state_["timer"] = action["result"];
        return;
    }
    if (type == "step") {
        Require(State()["mode"] == "independent", "Use independent mode for physical-channel stepping");
        const bool voltage = command["quantity"] == "voltage";
        const std::string prefix = voltage ? "VOLT" : "CURR";
        Json expected = Json::array();
        for (auto ch : command["channels"]) {
            Select(ch);
            double target = Number(prefix + "?") + command["direction"].get<int>() * Number(prefix + ":STEP?");
            Require(target >= 0 && target <= (voltage   ? Number("VOLT:LIMIT?")
                                              : ch == 3 ? 3
                                                        : 6),
                    "Step exceeds physical channel range");
            expected.push_back({{"channel", ch}, {"value", target}});
        }
        for (auto item : expected) {
            Select(item["channel"]);
            Command(prefix + (command["direction"] == 1 ? ":UP" : ":DOWN"));
            CheckScpi();
            Require(std::abs(Number(prefix + "?") - item["value"].get<double>()) < 0.02, "Step readback differs");
        }
        Poll();
        return;
    }
    if (type == "trigger") {
        const auto mode = State().value("mode", std::string("independent"));
        Require(mode == "independent", "Use independent mode for physical-channel trigger settings");
        std::string channels;
        for (auto ch : command["channels"]) {
            Select(ch);
            Command("VOLT:STEP " + Decimal(command["voltageStep"]));
            Command("CURR:STEP " + Decimal(command["currentStep"]));
            Command("VOLT:TRIG " + Decimal(command["voltage"]));
            Command("CURR:TRIG " + Decimal(command["current"]));
            if (!channels.empty())
                channels += ",";
            channels += "CH" + std::to_string(ch.get<int>());
        }
        Command("TRIG:DEL " + Decimal(command["delay"]));
        Command("INST:COUP " + channels);
        CheckScpi();
        if (command.value("fire", false)) {
            Command("*TRG");
            CheckScpi();
        }
        action["result"] = {{"delay", Number("TRIG:DEL?")}};
        Poll();
        return;
    }
    const auto op = command.at("operation").get<std::string>();
    if (op == "readTimer") {
        Json timer = {{"enabled", Number("OUTP:TIM?") != 0}, {"seconds", Number("OUTP:TIM:DEL?")}};
        std::lock_guard<std::mutex> lock(mutex_);
        state_["timer"] = timer;
        action["result"] = timer;
        return;
    }
    if (op == "readMode") {
        ReadMode();
        Poll();
        return;
    }
    if (op == "readProtection" || op == "protection" || op == "clearProtection") {
        if (op == "protection")
            for (auto ch : command["channels"]) {
                Select(ch);
                Require(command["ovp"].get<double>() <= Number("VOLT:PROT? MAX"), "OVP outside device range");
            }
        Json items = Json::array();
        for (auto ch : command["channels"]) {
            Select(ch);
            if (op == "protection") {
                Command("VOLT:LIMIT " + Decimal(command["limit"]));
                Command("VOLT:PROT " + Decimal(command["ovp"]));
                Command(command["enabled"].get<bool>() ? "VOLT:PROT:STAT ON" : "VOLT:PROT:STAT OFF");
                CheckScpi();
                Require(std::abs(Number("VOLT:LIMIT?") - command["limit"].get<double>()) < 0.02 &&
                            std::abs(Number("VOLT:PROT?") - command["ovp"].get<double>()) < 0.02 &&
                            (Number("VOLT:PROT:STAT?") != 0) == command["enabled"].get<bool>(),
                        "Protection readback differs");
            }
            if (op == "clearProtection") {
                Json result;
                auto mode = State().value("mode", std::string("independent"));
                Outputs(ch != 3 && mode != "independent" ? Json::array({1, 2}) : Json::array({ch}), false, result);
                Select(ch);
                Command("VOLT:PROT:CLEAR");
                CheckScpi();
            }
            Json item = {{"channel", ch},
                         {"limit", Number("VOLT:LIMIT?")},
                         {"ovp", Number("VOLT:PROT?")},
                         {"enabled", Number("VOLT:PROT:STAT?") != 0},
                         {"tripped", Number("VOLT:PROT:TRIP?") != 0}};
            if (op == "protection") {
                std::lock_guard<std::mutex> lock(mutex_);
                state_["channels"][ch.get<int>() - 1]["protection"] = {
                    {"enabled", command["softwareVoltage"] > 0 || command["softwareCurrent"] > 0},
                    {"voltageLimit", command["softwareVoltage"] == 0 ? 1e9 : command["softwareVoltage"].get<double>()},
                    {"currentLimit", command["softwareCurrent"] == 0 ? 1e9 : command["softwareCurrent"].get<double>()}};
                item["softwareVoltage"] = command["softwareVoltage"];
                item["softwareCurrent"] = command["softwareCurrent"];
            } else {
                auto protect = State()["channels"][ch.get<int>() - 1].value("protection", Json::object());
                item["softwareVoltage"] = protect.value("voltageLimit", 0.0);
                item["softwareCurrent"] = protect.value("currentLimit", 0.0);
                if (item["softwareVoltage"] == 1e9)
                    item["softwareVoltage"] = 0;
                if (item["softwareCurrent"] == 1e9)
                    item["softwareCurrent"] = 0;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_["channels"][ch.get<int>() - 1]["deviceProtection"] = item;
            }
            items.push_back(item);
        }
        action["result"] = items;
        return;
    }
    if (op == "panel") {
        if (command["control"] == "local")
            Command("SYST:LOC");
        else
            Command(command["locked"].get<bool>() ? "SYST:RWL" : "SYST:REM");
        Command(command["display"].get<bool>() ? "DISP ON" : "DISP OFF");
        if (command["text"].get<std::string>().empty())
            Command("DISP:TEXT:CLEAR");
        else
            Command("DISP:TEXT \"" + command["text"].get<std::string>() + "\"");
        CheckScpi();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_["panel"] = {{"control", command["control"]},
                               {"locked", command["control"] == "remote" && command["locked"].get<bool>()}};
        }
    }
    if (op == "panel" || op == "readPanel") {
        action["result"] = {{"display", Number("DISP?") != 0}, {"text", Query("DISP:TEXT?")}};
        std::lock_guard<std::mutex> lock(mutex_);
        state_["panel"].update(action["result"]);
        return;
    }
    if (op == "beep" || op == "clearText") {
        Command(op == "beep" ? "SYST:BEEP" : "DISP:TEXT:CLEAR");
        CheckScpi();
        return;
    }
    if (op == "savePreset") {
        Command("*SAV " + std::to_string(command["slot"].get<int>()));
        CheckScpi();
        return;
    }
    if (op == "recallPreset" || op == "reset") {
        Json off;
        Outputs(Json::array({1, 2, 3}), false, off);
        Command(op == "reset" ? "*RST" : "*RCL " + std::to_string(command["slot"].get<int>()));
        CheckScpi();
        ReadMode();
        Outputs(Json::array({1, 2, 3}), false, off);
        Poll();
        return;
    }
    if (op == "identity")
        action["result"] = {{"identity", Query("*IDN?")}, {"scpiVersion", Query("SYST:VERS?")}};
    if (op == "errors") {
        action["result"] = Json::array();
        for (int n = 0; n < 32; ++n) {
            auto error = Query("SYST:ERR?");
            action["result"].push_back(error);
            std::istringstream input(error);
            int code = 0;
            input >> code;
            Require(!input.fail(), "Invalid error queue response");
            if (code == 0)
                break;
        }
    }
    if (op == "status")
        action["result"] = {{"statusByte", Number("*STB?")},
                            {"questionable", Number("STAT:QUES:COND?")},
                            {"operation", Number("STAT:OPER:COND?")}};
    if (op == "selfTest") {
        Json off;
        Outputs(Json::array({1, 2, 3}), false, off);
        action["result"] = Number("*TST?");
    }
    if (op == "scpi") {
        Json off;
        Outputs(Json::array({1, 2, 3}), false, off);
        const auto line = command["line"].get<std::string>();
        try {
            if (line.find('?') != std::string::npos)
                action["result"] = Query(line);
            else {
                Command(line);
                CheckScpi();
                action["result"] = "Command accepted";
            }
            ReadMode();
            Outputs(Json::array({1, 2, 3}), false, off);
            Poll();
        } catch (...) {
            try {
                Command("OUTP OFF");
            } catch (...) {
            }
            throw;
        }
    }
}
void PowerService::AdvanceTask() {
    const auto type = task_.value("type", std::string("task"));
    Json result;
    if (type == "sequence") {
        int index = task_["index"];
        if (index >= static_cast<int>(task_["steps"].size())) {
            StopTask();
            return;
        }
        auto step = task_["steps"][index];
        Outputs(owned_, false, result);
        Json settings = Json::array();
        const auto mode = State().value("mode", std::string("independent"));
        bool grouped = mode != "independent" && std::find(owned_.begin(), owned_.end(), Json(1)) != owned_.end();
        if (grouped)
            settings.push_back(
                {{"channels", Json::array({1, 2})}, {"voltage", step["voltage"]}, {"current", step["current"]}});
        for (auto ch : owned_) {
            if (grouped && ch != 3)
                continue;
            settings.push_back(
                {{"channels", Json::array({ch})}, {"voltage", step["voltage"]}, {"current", step["current"]}});
        }
        SetParameters(settings);
        Outputs(owned_, step["enabled"], result);
        task_["index"] = index + 1;
        task_["completed"] = index + 1;
        taskAt_ = GetTickCount64() + step["durationMs"].get<std::uint64_t>();
    } else if (type == "schedule") {
        if (task_["phase"] == "on") {
            StopTask();
            return;
        }
        Outputs(owned_, true, result);
        task_["phase"] = "on";
        taskAt_ = GetTickCount64() + task_["durationMs"].get<std::uint64_t>();
    } else {
        bool on = task_["phase"] == "off";
        Outputs(owned_, on, result);
        task_["phase"] = on ? "on" : "off";
        taskAt_ = GetTickCount64() + task_[on ? "onMs" : "offMs"].get<std::uint64_t>();
        if (!on) {
            task_["completed"] = task_["completed"].get<int>() + 1;
            if (task_["completed"] >= task_["count"]) {
                StopTask();
                return;
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_["task"] = task_;
    state_["task"]["running"] = true;
}
} // namespace serialctl
