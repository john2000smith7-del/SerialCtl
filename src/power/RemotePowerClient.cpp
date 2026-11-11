#include "RemotePowerClient.h"
#include "RemotePowerProtocol.h"
#include "Win32Helpers.h"
#include <future>

namespace serialctl {
namespace {
using Json = nlohmann::json;
constexpr auto Resource = "power-1";
constexpr DWORD NetworkBudgetMs = 1800;

std::string ErrorMessage(const std::exception &error) {
    const auto reply = Json::parse(error.what(), nullptr, false);
    if (reply.is_object() && reply.contains("message") && reply["message"].is_string())
        return reply["message"];
    return error.what();
}

Json PowerSnapshot(const Json &response) {
    auto state = response.at("powerState");
    const auto backend = state.at("backend").get<std::string>();
    if (!state.at("connected").get<bool>() || (backend != "usb" && backend != "serial") ||
        state.at("connectionId").get<std::string>().empty() || state.at("mode").get<std::string>().empty())
        throw std::runtime_error(u8"远端没有已连接的本地电源");
    const auto &channels = state.at("channels");
    const auto &task = state.at("task");
    if (!channels.is_array() || channels.size() != 3 ||
        !std::all_of(channels.begin(), channels.end(), [](const Json &channel) {
            const auto protection = channel.value("deviceProtection", Json::object());
            return protection.is_object() &&
                   (!protection.contains("enabled") || protection["enabled"].is_boolean());
        }) || !task.is_object())
        throw std::runtime_error(u8"远端电源状态格式无效");
    (void)task.at("running").get<bool>();
    (void)task.value("type", std::string());
    (void)task.value("phase", std::string());
    (void)task.value("completed", 0);
    (void)task.value("count", 1);
    (void)task.value("remainingMs", std::uint64_t{});
    (void)state.value("identity", std::string());
    (void)state.value("error", std::string());
    (void)state.value("readbackStale", false);
    const auto panel = state.value("panel", Json::object());
    if (!panel.is_object()) throw std::runtime_error(u8"远端电源面板状态格式无效");
    (void)panel.value("display", false);
    (void)panel.value("text", std::string());
    const auto timer = state.value("timer", Json::object());
    if (!timer.is_object()) throw std::runtime_error(u8"远端电源定时器状态格式无效");
    (void)timer.value("enabled", false);
    auto records = state.value("records", Json::array());
    if (!records.is_array()) throw std::runtime_error(u8"远端电源记录格式无效");
    for (auto &record : records) {
        (void)record.value("timestamp", std::string());
        (void)record.value("operation", std::string());
        (void)record.value("result", std::string());
        (void)record.value("source", std::string());
        if (record.contains("unconfirmedChannels"))
            (void)record.at("unconfirmedChannels").get<std::vector<int>>();
    }
    // A connected host's readbacks, not local file paths, drive this pane.
    for (const char *key : {"logPath", "csvPath", "lastDisconnectActionId",
                            "unconfirmedPowerOff", "unconfirmedPowerOffChannels"})
        state.erase(key);
    return state;
}
bool Pending(const Json &action) {
    return action["state"] == "queued" || action["state"] == "running";
}
} // namespace

RemotePowerTarget RemotePowerClient::Discover(const std::wstring &host, const std::atomic_bool &cancel) {
    IN_ADDR ip{};
    if (InetPtonW(AF_INET, host.c_str(), &ip) != 1 || ip.s_addr == INADDR_ANY)
        throw std::runtime_error(u8"请输入远端电脑的 IPv4 地址");
    const auto deadline = GetTickCount64() + NetworkBudgetMs;
    TcpAddresses addresses;
    std::wstring error;
    if (!ResolveTcpAddresses(host, deadline, addresses, error, &cancel))
        throw std::runtime_error(WideToMultiByte(error, CP_UTF8));
    struct Probe {
        RemotePowerTarget target;
        std::string error;
    };
    std::vector<std::future<Probe>> probes;
    for (unsigned port = 7000; port <= 7015; ++port) {
        probes.push_back(std::async(std::launch::async, [&, port] {
            GatewayClient gateway;
            Probe probe;
            try {
                gateway.ConnectUntil(host, port, addresses, &cancel, deadline);
                probe.target.instance = gateway.Hello().at("instance").get<std::string>();
                const auto state = PowerSnapshot(gateway.Request(RemotePowerStateOp, Resource));
                probe.target = {host, port, probe.target.instance, state.at("connectionId"), state.at("backend")};
            } catch (const std::exception &e) {
                if (!probe.target.instance.empty()) probe.error = ErrorMessage(e);
            }
            return probe;
        }));
    }
    RemotePowerTarget target;
    std::string lastError;
    unsigned candidates = 0;
    for (auto &probe : probes) {
        auto result = probe.get();
        if (!result.error.empty()) lastError = std::move(result.error);
        if (result.target.port) {
            target = std::move(result.target);
            ++candidates;
        }
    }
    if (cancel) throw std::runtime_error(u8"刷新已取消");
    if (candidates > 1) throw std::runtime_error(u8"同一 IP 上发现多个可控制的电源实例");
    if (!candidates) throw std::runtime_error(lastError.empty() ? u8"未发现远端 SerialCtl（端口 7000–7015）" : lastError);
    return target;
}

RemotePowerClient::~RemotePowerClient() {
    Detach();
    Join();
}
void RemotePowerClient::Attach(RemotePowerTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!finished_) throw std::runtime_error(u8"上一远程连接正在退出，请稍后重试");
    Join();
    cancel_ = false;
    finished_ = false;
    active_ = true;
    state_ = {{"connected", false}, {"task", {{"running", false}}}, {"remoteHost", WideToMultiByte(target.host, CP_UTF8)}};
    try {
        worker_ = std::thread([this, target = std::move(target)] { Run(target); });
    } catch (...) {
        finished_ = true;
        active_ = false;
        state_ = nullptr;
        throw;
    }
}
void RemotePowerClient::Detach() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_ = true;
        active_ = false;
        state_ = nullptr;
        wake_.notify_one();
    }
    gateway_.Cancel();
}
void RemotePowerClient::Join() {
    if (worker_.joinable()) worker_.join();
}
bool RemotePowerClient::Active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}
Json RemotePowerClient::State() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
Json RemotePowerClient::Submit(const Json &command, const std::string &mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || !state_.value("connected", false))
        return {{"error", u8"远程电源未连接"}};
    if (!RemotePowerCommandAllowed(command))
        return {{"error", u8"此操作仅支持在电源所在电脑执行"}};
    if (std::count_if(actions_.begin(), actions_.end(), [](const auto &entry) { return Pending(entry.second); }) >= 16)
        return {{"error", u8"远程电源命令队列已满"}};
    // Keep recent results until the pane has observed them.
    if (actions_.size() >= 128) {
        const auto old = std::min_element(actions_.begin(), actions_.end(), [](const auto &a, const auto &b) {
            return std::make_pair(Pending(a.second), a.second.at("serial").template get<std::uint64_t>()) <
                   std::make_pair(Pending(b.second), b.second.at("serial").template get<std::uint64_t>());
        });
        actions_.erase(old);
    }
    const auto serial = ++next_;
    auto id = "remote-power-" + std::to_string(serial);
    auto action = Json{{"id", id}, {"state", "queued"}, {"command", command}, {"serial", serial},
                       {"mode", mode}};
    actions_[id] = action;
    queued_.push_back(id);
    wake_.notify_one();
    return action;
}
Json RemotePowerClient::Action(const std::string &id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = actions_.find(id);
    return entry == actions_.end() ? Json{{"state", "failed"}, {"error", u8"远程操作记录已过期"}} : entry->second;
}
void RemotePowerClient::Run(const RemotePowerTarget &target) {
    std::deque<std::pair<std::string, std::string>> awaiting;
    std::string error;
    auto request = [&](const char *operation, Json params = Json::object()) {
        return gateway_.RequestUntil(operation, Resource, std::move(params), GetTickCount64() + NetworkBudgetMs);
    };
    auto poll = [&] {
        auto snapshot = PowerSnapshot(request(RemotePowerStateOp));
        if (snapshot.at("connectionId") != target.connectionId)
            throw std::runtime_error(u8"远端电源已重新连接，请刷新后重新选择");
        snapshot["remoteHost"] = WideToMultiByte(target.host, CP_UTF8);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancel_) state_ = std::move(snapshot);
    };
    try {
        gateway_.ConnectUntil(target.host, target.port, &cancel_, GetTickCount64() + NetworkBudgetMs);
        if (gateway_.Hello().at("instance") != target.instance)
            throw std::runtime_error(u8"远端 SerialCtl 实例已变化，请重新刷新");
        poll();
        auto nextPoll = GetTickCount64() + 500;
        while (!cancel_) {
            Json action;
            std::string id;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(125), [this] { return cancel_ || !queued_.empty(); });
                if (cancel_) break;
                if (!queued_.empty()) {
                    id = std::move(queued_.front());
                    queued_.pop_front();
                    actions_.at(id)["state"] = "running";
                    action = actions_.at(id);
                }
            }
            if (!id.empty() && !cancel_) {
                const auto result = request(RemotePowerSubmitOp, {{"connectionId", target.connectionId},
                                            {"mode", action.at("mode")}, {"command", action.at("command")}}).at("action");
                if (result.contains("error")) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    actions_.at(id)["state"] = "failed";
                    actions_.at(id)["error"] = result.at("error").get<std::string>();
                } else {
                    awaiting.emplace_back(id, result.at("id").get<std::string>());
                }
            }
            if (!awaiting.empty() && !cancel_) {
                auto pending = std::move(awaiting.front());
                awaiting.pop_front();
                const auto result = request(RemotePowerActionOp, {{"id", pending.second}}).at("action");
                const auto status = result.at("state").get<std::string>();
                if (status == "queued" || status == "running") {
                    awaiting.push_back(std::move(pending));
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto &local = actions_.at(pending.first);
                    local["state"] = status == "completed" || status == "canceled" ? status : "failed";
                    if (result.contains("error")) local["error"] = result.at("error").get<std::string>();
                    if (result.contains("result")) local["result"] = result.at("result");
                    nextPoll = 0;
                }
            }
            if (!cancel_ && GetTickCount64() >= nextPoll) {
                poll();
                nextPoll = GetTickCount64() + 500;
            }
        }
    } catch (const std::exception &e) {
        error = ErrorMessage(e);
    }
    gateway_.Close();
    std::lock_guard<std::mutex> lock(mutex_);
    queued_.clear();
    for (auto &entry : actions_) {
        auto &action = entry.second;
        if (action["state"] == "queued") action["state"] = "canceled";
        else if (action["state"] == "running") {
            action["state"] = "failed";
            action["error"] = u8"远程连接已断开，已提交操作的结果未知，请查看电源所在电脑";
        }
    }
    if (!cancel_) {
        state_ = {{"connected", false}, {"task", {{"running", false}}},
                  {"remoteHost", WideToMultiByte(target.host, CP_UTF8)}, {"error", error},
                  {"remoteLost", true}};
    }
    active_ = false;
    finished_ = true;
}
} // namespace serialctl
