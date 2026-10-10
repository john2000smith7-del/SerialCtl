#pragma once
#include "Connection.h"
#include "json.hpp"
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
namespace serialctl
{
using Json = nlohmann::json;
struct ApiGrant
{
    unsigned rights = 1;
    unsigned channels = 7;
};
class ApiServer
{
  public:
    using Handler = std::function<Json(const std::string &, const std::string &, const Json &)>;
    ApiServer();
    ~ApiServer();
    bool Start(Handler handler, std::map<std::string, ApiGrant> grants, std::wstring &error);
    void Stop();
    bool Running() const
    {
        return running_;
    }
    unsigned Port() const
    {
        return port_;
    }
    std::string Token() const;
    std::string Instance() const
    {
        return instance_;
    }
    void Publish(const std::string &resource, const Bytes &bytes, const char *source = "device",
                 const char *kind = "output");
    Json Events(const std::string &resource, std::uint64_t after);

  private:
    void Accept();
    void Client(SOCKET);
    void Discover();
    Json Call(const std::string &method, const std::string &path, const Json &body);
    bool Allowed(const std::string &method, const std::string &path, const Json &body);
    void Websocket(SOCKET, const std::string &resource, const std::string &key);
    mutable std::mutex mutex_;
    std::map<std::string, ApiGrant> grants_;
    std::deque<Json> events_;
    std::uint64_t sequence_ = 0;
    std::map<SOCKET, std::thread> clients_;
    std::set<SOCKET> finished_;
    std::atomic_bool running_{false};
    std::atomic<SOCKET> listen_{INVALID_SOCKET}, discovery_{INVALID_SOCKET};
    unsigned port_ = 0;
    std::string token_, instance_;
    Handler handler_;
    std::thread accept_, discoveryThread_;
};
std::string Encode64(const Bytes &bytes);
Bytes Decode64(const std::string &text);
} // namespace serialctl
