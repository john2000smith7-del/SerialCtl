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
namespace serialctl
{
namespace
{
using Vi = unsigned long;
using Vs = long;
struct Visa
{
    HMODULE dll = nullptr;
    Vs(WINAPI *openRM)(Vi *) = nullptr;
    Vs(WINAPI *find)(Vi, const char *, Vi *, Vi *, char *) = nullptr;
    Vs(WINAPI *next)(Vi, char *) = nullptr;
    Vs(WINAPI *open)(Vi, const char *, Vi, Vi, Vi *) = nullptr;
    Vs(WINAPI *close)(Vi) = nullptr;
    Vs(WINAPI *attr)(Vi, Vi, std::uint64_t) = nullptr;
    Vs(WINAPI *write)(Vi, const unsigned char *, Vi, Vi *) = nullptr;
    Vs(WINAPI *read)(Vi, unsigned char *, Vi, Vi *) = nullptr;
    Visa()
    {
        wchar_t sys[MAX_PATH]{};
        GetSystemDirectoryW(sys, MAX_PATH);
        dll = LoadLibraryW((std::wstring(sys) + L"\\visa64.dll").c_str());
        if (!dll)
            return;
#define VISA_PROC(member, name)                                                                    \
    member = reinterpret_cast<decltype(member)>(GetProcAddress(dll, name))
        VISA_PROC(openRM, "viOpenDefaultRM");
        VISA_PROC(find, "viFindRsrc");
        VISA_PROC(next, "viFindNext");
        VISA_PROC(open, "viOpen");
        VISA_PROC(close, "viClose");
        VISA_PROC(attr, "viSetAttribute");
        VISA_PROC(write, "viWrite");
        VISA_PROC(read, "viRead");
#undef VISA_PROC
        if (!openRM || !find || !next || !open || !close || !attr || !write || !read)
        {
            FreeLibrary(dll);
            dll = nullptr;
        }
    }
    ~Visa()
    {
        if (dll)
            FreeLibrary(dll);
    }
};
void Require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
std::string Decimal(double value)
{
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(12);
    text << value;
    return text.str();
}
Json ValidateChannels(const Json &command)
{
    Require(command.contains("channels") && command["channels"].is_array() &&
                !command["channels"].empty() && command["channels"].size() <= 3,
            "Select channels 1..3");
    Json channels = command["channels"];
    std::set<int> unique;
    for (const auto &item : channels)
    {
        Require(item.is_number_integer(), "Invalid channel");
        int channel = item.get<int>();
        Require(channel >= 1 && channel <= 3 && unique.insert(channel).second,
                "Invalid or duplicate channel");
    }
    return channels;
}
std::string Timestamp()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char text[40]{};
    sprintf_s(text, "%04u-%02u-%02u %02u:%02u:%02u.%03u", now.wYear, now.wMonth, now.wDay,
              now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return text;
}
} // namespace
struct PowerService::Transport
{
    Visa visa;
    Vi rm = 0, device = 0;
    HANDLE serial = INVALID_HANDLE_VALUE;
    bool simulated = false;
    int failAfter = -1;
    Json simulation = Json::array({Json{{"voltage", 0.0},
                                        {"current", 0.0},
                                        {"output", false},
                                        {"ovp", 30.0},
                                        {"ovpEnabled", false}},
                                   Json{{"voltage", 0.0},
                                        {"current", 0.0},
                                        {"output", false},
                                        {"ovp", 30.0},
                                        {"ovpEnabled", false}},
                                   Json{{"voltage", 0.0},
                                        {"current", 0.0},
                                        {"output", false},
                                        {"ovp", 5.5},
                                        {"ovpEnabled", false}}});
    int selected = 0;
    ~Transport()
    {
        if (device)
            visa.close(device);
        if (rm)
            visa.close(rm);
        if (serial != INVALID_HANDLE_VALUE)
            CloseHandle(serial);
    }
    void Write(std::string command)
    {
        if (simulated)
        {
            if (command.rfind("INST:NSEL ", 0) == 0)
                selected = std::stoi(command.substr(10)) - 1;
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
        if (device)
        {
            Vi written = 0;
            Require(visa.write(device, reinterpret_cast<const unsigned char *>(command.data()),
                               static_cast<Vi>(command.size()), &written) >= 0 &&
                        written == command.size(),
                    "USB write failed");
        }
        else
            Require(WriteFile(serial, command.data(), static_cast<DWORD>(command.size()), &count,
                              nullptr) &&
                        count == command.size(),
                    "Serial write failed");
    }
    std::string Query(std::string command)
    {
        if (simulated)
        {
            if (failAfter == 0)
                throw std::runtime_error("Simulated communication timeout");
            if (failAfter > 0)
                --failAfter;
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
        while (GetTickCount64() < end && result.size() < 4096)
        {
            unsigned char buffer[256]{};
            DWORD count = 0;
            bool endOfTransfer = false;
            if (device)
            {
                Vi got = 0;
                Vs status = visa.read(device, buffer, sizeof(buffer), &got);
                Require(status >= 0, "USB query timeout or read failure");
                count = got;
                endOfTransfer = status != 0x3FFF0006 && count > 0;
            }
            else
                Require(ReadFile(serial, buffer, sizeof(buffer), &count, nullptr) != FALSE,
                        "Serial read failed");
            result.append(reinterpret_cast<char *>(buffer), count);
            if (result.find('\n') != std::string::npos || (device && endOfTransfer))
            {
                while (!result.empty() && (result.back() == '\r' || result.back() == '\n'))
                    result.pop_back();
                return result;
            }
        }
        throw std::runtime_error("SCPI query timed out");
    }
};
bool PowerService::VisaAvailable()
{
    Visa visa;
    return visa.dll != nullptr;
}
Json PowerService::UsbResources()
{
    Visa visa;
    Json resources = Json::array();
    if (!visa.dll)
        return resources;
    Vi rm = 0, list = 0, count = 0;
    char name[512]{};
    if (visa.openRM(&rm) < 0)
        return resources;
    if (visa.find(rm, "USB?*INSTR", &list, &count, name) >= 0)
    {
        for (Vi i = 0; i < std::min<Vi>(count, 64); ++i)
        {
            if (i && visa.next(list, name) < 0)
                break;
            resources.push_back(name);
        }
        visa.close(list);
    }
    visa.close(rm);
    return resources;
}
PowerService::PowerService()
{
    state_ = {{"id", "power-1"},     {"connected", false},        {"model", "IT6332A"},
              {"simulation", false}, {"channels", Json::array()}, {"task", {{"running", false}}}};
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
PowerService::~PowerService()
{
    Shutdown();
}
Json PowerService::State()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
Json PowerService::Action(const std::string &id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = actions_.find(id);
    return it == actions_.end() ? Json{{"error", "Action expired or not found"}} : it->second;
}
Json PowerService::Submit(const Json &command)
{
    try
    {
        const std::string type = command.at("type").get<std::string>();
        Require(type == "connect" || type == "disconnect" || type == "output" ||
                    type == "parameters" || type == "protection" || type == "task" ||
                    type == "stop",
                "Unknown power action");
        if (type == "output" || type == "parameters" || type == "protection" || type == "task")
        {
            auto channels = ValidateChannels(command);
            if (type == "output")
                Require(command.at("enabled").is_boolean(), "Invalid output state");
            if (type == "parameters")
                for (const auto &c : channels)
                {
                    Require(command.at("voltage").is_number() && command.at("current").is_number(),
                            "Invalid voltage or current");
                    double v = command.at("voltage"), a = command.at("current");
                    int n = c.get<int>();
                    Require(std::isfinite(v) && std::isfinite(a) && v >= 0 && a >= 0 &&
                                v <= (n == 3 ? 5 : 30) && a <= (n == 3 ? 3 : 6),
                            "Voltage or current outside channel range");
                }
            if (type == "protection")
            {
                Require(command.at("enabled").is_boolean(), "Invalid protection state");
                for (const auto &c : channels)
                {
                    int n = c.get<int>();
                    double v = command.at("voltageLimit"), a = command.at("currentLimit");
                    Require(std::isfinite(v) && std::isfinite(a) && v > 0 && a > 0 &&
                                v <= (n == 3 ? 5 : 30) && a <= (n == 3 ? 3 : 6),
                            "Invalid protection thresholds");
                }
            }
            if (type == "task")
            {
                Require(command.at("onMs").is_number_unsigned() ||
                            command.at("onMs").is_number_integer(),
                        "Invalid timing");
                auto on = command.at("onMs").get<long long>(),
                     off = command.at("offMs").get<long long>(),
                     count = command.at("count").get<long long>();
                Require(on >= 100 && off >= 100 && on <= 604800000 && off <= 604800000 &&
                            count >= 1 && count <= 1000000,
                        "Invalid task timings/count");
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        Require(!stopping_ && queue_.size() < 128, "Power command queue full or shutting down");
        if (command.contains("requestId"))
        {
            Require(command["requestId"].is_string() &&
                        command["requestId"].get<std::string>().size() <= 128,
                    "Invalid request ID");
            for (const auto &action : actions_)
                if (action.second.value("requestId", std::string()) ==
                    command["requestId"].get<std::string>())
                {
                    Require(action.second["command"] == command,
                            "Request ID reused with different parameters");
                    return action.second;
                }
        }
        std::string id = "power-action-" + std::to_string(next_++);
        Json action = {{"id", id},
                       {"state", "queued"},
                       {"command", command},
                       {"requestId", command.value("requestId", std::string())}};
        actions_[id] = action;
        const bool priority = type == "stop" || type == "disconnect" ||
                              (type == "output" && !command["enabled"].get<bool>());
        if (priority)
        {
            for (auto it = queue_.begin(); it != queue_.end();)
            {
                const auto t = it->second.value("type", std::string());
                if (t == "task" || (t == "output" && it->second.value("enabled", false)))
                {
                    actions_[it->first]["state"] = "canceled";
                    it = queue_.erase(it);
                }
                else
                    ++it;
            }
            queue_.emplace_front(id, command);
        }
        else
            queue_.emplace_back(id, command);
        while (actions_.size() > 256)
        {
            auto it = std::find_if(actions_.begin(), actions_.end(), [](const auto &item) {
                return item.second["state"] != "queued" && item.second["state"] != "running";
            });
            if (it == actions_.end())
                break;
            actions_.erase(it);
        }
        wake_.notify_one();
        return action;
    }
    catch (const std::exception &e)
    {
        return {{"error", e.what()}};
    }
}
void PowerService::Command(const std::string &command)
{
    Require(transport_ != nullptr, "Power supply disconnected");
    transport_->Write(command);
}
std::string PowerService::Query(const std::string &command)
{
    Require(transport_ != nullptr, "Power supply disconnected");
    return transport_->Query(command);
}
double PowerService::Number(const std::string &command)
{
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
void PowerService::Select(int channel)
{
    Command("INST:NSEL " + std::to_string(channel));
    Require(Number("INST:NSEL?") == channel, "Channel selection readback differs");
}
void PowerService::Outputs(const Json &channels, bool enabled, Json &result)
{
    result = Json::array();
    bool failed = false;
    for (const auto &c : channels)
    {
        int channel = c.get<int>();
        Json item = {{"channel", channel}, {"state", "skipped"}};
        if (!failed || !enabled)
            try
            {
                if (failed)
                {
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
            }
            catch (const std::exception &e)
            {
                failed = true;
                item["state"] = "unknown";
                item["error"] = e.what();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_["channels"][channel - 1]["output"] = nullptr;
                }
            }
        result.push_back(item);
    }
    log_.WriteStatus(MultiByteToWide(reinterpret_cast<const std::uint8_t *>(result.dump().data()),
                                     result.dump().size(), CP_UTF8));
    if (failed)
        throw std::runtime_error("Output operation incomplete; inspect per-channel result");
}
void PowerService::StopTask()
{
    if (owned_.empty())
        return;
    Json channels = owned_;
    task_ = Json();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_["task"]["running"] = false;
    }
    if (transport_)
    {
        Json result;
        Outputs(channels, false, result);
    }
    owned_ = Json::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_["ownedChannels"] = owned_;
    }
}
void PowerService::Disconnect()
{
    try
    {
        StopTask();
    }
    catch (const std::exception &e)
    {
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
    for (auto &channel : state_["channels"])
    {
        channel["output"] = nullptr;
        channel["voltage"] = nullptr;
        channel["current"] = nullptr;
        channel["power"] = nullptr;
    }
}
void PowerService::Execute(const std::string &id, const Json &command)
{
    Json action;
    bool communication = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actions_[id]["state"] = "running";
        action = actions_[id];
    }
    try
    {
        std::string type = command["type"];
        if (type == "connect")
        {
            Require(!transport_, "Already connected");
            auto transport = std::make_unique<Transport>();
            std::string backend = command.at("backend");
            if (backend == "simulation")
            {
                transport->simulated = true;
                transport->failAfter = command.value("testFailAfterReads", -1);
            }
            else if (backend == "usb")
            {
                Require(transport->visa.dll != nullptr, "Install bundled VISA runtime first");
                Require(transport->visa.openRM(&transport->rm) >= 0, "VISA unavailable");
                const auto resource = command.at("resource").get<std::string>();
                Require(resource.rfind("USB", 0) == 0 && resource.size() < 512,
                        "Invalid USB resource");
                Require(transport->visa.open(transport->rm, resource.c_str(), 0, 2500,
                                             &transport->device) >= 0,
                        "Cannot open USB supply");
                Require(transport->visa.attr(transport->device, 0x3FFF001A, 2500) >= 0,
                        "Cannot set USB timeout");
            }
            else if (backend == "serial")
            {
                std::wstring port = MultiByteToWide(
                    reinterpret_cast<const std::uint8_t *>(
                        command.at("port").get_ref<const std::string &>().data()),
                    command.at("port").get_ref<const std::string &>().size(), CP_UTF8);
                Require(port.rfind(L"COM", 0) == 0 && port.size() > 3 && port.size() <= 12 &&
                            port.find_first_not_of(L"0123456789", 3) == std::wstring::npos,
                        "Invalid COM name");
                transport->serial =
                    CreateFileW((L"\\\\.\\" + port).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING, 0, nullptr);
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
                Require(SetCommState(transport->serial, &dcb) != FALSE,
                        "Invalid serial configuration");
                COMMTIMEOUTS timeouts{MAXDWORD, 0, 250, 0, 2500};
                Require(SetCommTimeouts(transport->serial, &timeouts) != FALSE,
                        "Cannot set COM timeout");
                PurgeComm(transport->serial, PURGE_RXCLEAR | PURGE_TXCLEAR);
            }
            else
                throw std::runtime_error("Unknown connection backend");
            std::string identity = transport->Query("*IDN?");
            auto comma = identity.find(',');
            auto nextComma =
                comma == std::string::npos ? std::string::npos : identity.find(',', comma + 1);
            std::string model = comma == std::string::npos
                                    ? std::string()
                                    : identity.substr(comma + 1, nextComma - comma - 1);
            model.erase(std::remove_if(model.begin(), model.end(),
                                       [](unsigned char c) { return std::isspace(c) != 0; }),
                        model.end());
            Require(model == "IT6332A", "Instrument is not IT6332A");
            transport_ = std::move(transport);
            Select(1);
            Require(Number("OUTP:TRAC?") == 0 && Number("OUTP:SER?") == 0 &&
                        Number("OUTP:PAR?") == 0,
                    "Disable tracking/series/parallel mode on the instrument before independent "
                    "control");
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
                state_["csvPath"] = WideToMultiByte(measurements_.Path(), CP_UTF8);
                if (!csvError.empty())
                    state_["logError"] = WideToMultiByte(csvError, CP_UTF8);
                state_["logPath"] = WideToMultiByte(log_.Path(), CP_UTF8);
                if (!error.empty())
                    state_["logError"] = WideToMultiByte(error, CP_UTF8);
            }
            Poll();
            pollAt_ = GetTickCount64() + 1000;
        }
        else if (type == "disconnect")
            Disconnect();
        else if (type == "stop")
            StopTask();
        else if (type == "output")
        {
            if (!command["enabled"].get<bool>())
            {
                task_ = Json();
                std::lock_guard<std::mutex> lock(mutex_);
                state_["task"]["running"] = false;
            }
            communication = true;
            Outputs(command["channels"], command["enabled"], action["channels"]);
        }
        else if (type == "parameters")
        {
            Require(task_.is_null() || task_.empty(),
                    "Stop the running task before changing parameters");
            communication = true;
            Json result = Json::array();
            for (const auto &c : command["channels"])
            {
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
        }
        else if (type == "protection")
        {
            Require(task_.is_null() || task_.empty(), "Stop the task before changing protection");
            communication = true;
            // Preflight every hardware limit before changing any selected channel.
            for (const auto &c : command["channels"])
            {
                Select(c.get<int>());
                Require(command["voltageLimit"].get<double>() <= Number("VOLT:PROT? MAX"),
                        "OVP threshold exceeds instrument range");
            }
            for (const auto &c : command["channels"])
            {
                int channel = c.get<int>();
                Select(channel);
                Command("VOLT:PROT " + Decimal(command["voltageLimit"]));
                Command(command["enabled"].get<bool>() ? "VOLT:PROT:STAT ON"
                                                       : "VOLT:PROT:STAT OFF");
                Require(std::abs(Number("VOLT:PROT?") - command["voltageLimit"].get<double>()) <
                                0.02 &&
                            (Number("VOLT:PROT:STAT?") != 0) == command["enabled"].get<bool>(),
                        "Protection readback differs");
                std::lock_guard<std::mutex> lock(mutex_);
                state_["channels"][channel - 1]["protection"] = {
                    {"enabled", command["enabled"]},
                    {"voltageLimit", command["voltageLimit"]},
                    {"currentLimit", command["currentLimit"]}};
            }
        }
        else if (type == "task")
        {
            Require(transport_ != nullptr, "Supply disconnected");
            Require(task_.is_null() || task_.empty(), "A task is already running");
            Require(owned_.empty(), "Stop and power off the previous canceled task first");
            task_ = command;
            owned_ = command["channels"];
            task_["completed"] = 0;
            task_["phase"] = "off";
            taskAt_ = GetTickCount64();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_["task"] = task_;
                state_["task"]["running"] = true;
                state_["ownedChannels"] = owned_;
            }
        }
        action["state"] = "completed";
    }
    catch (const std::exception &e)
    {
        if (command.value("type", std::string()) == "connect" || communication)
        {
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
    log_.WriteStatus(MultiByteToWide(reinterpret_cast<const std::uint8_t *>(action.dump().data()),
                                     action.dump().size(), CP_UTF8));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actions_[id] = action;
    }
}
void PowerService::Poll()
{
    Json channels = State()["channels"];
    for (int i = 0; i < 3; ++i)
    {
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
                          Decimal(c["voltage"]) + "," + Decimal(c["current"]) + "," +
                          Decimal(c["power"]) + "," + (c["output"].get<bool>() ? "ON" : "OFF") +
                          "\r\n";
        measurements_.WriteRaw(Bytes(csv.begin(), csv.end()));
        c["ovpTripped"] = Number("VOLT:PROT:TRIP?") != 0;
        if (c["ovpTripped"].get<bool>() || c["output"].get<bool>())
        {
            bool fault = c["ovpTripped"].get<bool>();
            if (c.contains("protection") && c["protection"].value("enabled", false))
            {
                fault =
                    fault ||
                    c["voltage"].get<double>() > c["protection"]["voltageLimit"].get<double>() ||
                    c["current"].get<double>() > c["protection"]["currentLimit"].get<double>();
            }
            if (fault)
            {
                StopTask();
                task_ = Json();
                Json off;
                Outputs(Json::array({i + 1}), false, off);
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
void PowerService::Run()
{
    for (;;)
    {
        std::pair<std::string, Json> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(50),
                           [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
                break;
            if (!queue_.empty())
            {
                job = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (!job.first.empty())
            Execute(job.first, job.second);
        if (!transport_)
            continue;
        try
        {
            auto now = GetTickCount64();
            if (!task_.is_null() && !task_.empty() && now >= taskAt_)
            {
                bool on = task_["phase"] == "off";
                Json result;
                Outputs(task_["channels"], on, result);
                task_["phase"] = on ? "on" : "off";
                taskAt_ = GetTickCount64() + task_[on ? "onMs" : "offMs"].get<std::uint64_t>();
                if (!on)
                {
                    task_["completed"] = task_["completed"].get<int>() + 1;
                    if (task_["completed"].get<int>() >= task_["count"].get<int>())
                    {
                        StopTask();
                        continue;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_["task"] = task_;
                    state_["task"]["running"] = true;
                }
            }
            if (now >= pollAt_)
            {
                Poll();
                pollAt_ = GetTickCount64() + 1000;
            }
        }
        catch (const std::exception &e)
        {
            std::string error = e.what();
            Disconnect();
            std::lock_guard<std::mutex> lock(mutex_);
            state_["error"] = error;
        }
    }
    Disconnect();
}
void PowerService::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
        wake_.notify_all();
    }
    if (thread_.joinable())
        thread_.join();
}
} // namespace serialctl
