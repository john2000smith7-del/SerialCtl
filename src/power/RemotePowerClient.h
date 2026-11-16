#pragma once
#include "GatewayClient.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

namespace serialctl {
struct RemotePowerTarget {
    std::wstring host;
    unsigned port = 0;
    std::string instance;
    std::string connectionId;
    std::string backend;
};

class RemotePowerClient {
public:
    ~RemotePowerClient();
    static RemotePowerTarget Discover(const std::wstring &host, const std::atomic_bool &cancel);
    void Attach(RemotePowerTarget target);
    void Detach();
    void Join();
    bool Active() const;
    nlohmann::json State() const;
    nlohmann::json Submit(const nlohmann::json &command, const std::string &mode);
    nlohmann::json Action(const std::string &id) const;

private:
    using Json = nlohmann::json;
    void Run(const RemotePowerTarget &target);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::atomic_bool cancel_{false};
    bool active_ = false;
    bool finished_ = true;
    Json state_;
    std::uint64_t next_ = 0;
    std::deque<std::string> queued_;
    std::map<std::string, Json> actions_;
    GatewayClient gateway_;
};
} // namespace serialctl
