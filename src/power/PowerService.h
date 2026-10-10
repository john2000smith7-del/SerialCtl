#pragma once
#include "Connection.h"
#include "SessionLogger.h"
#include "json.hpp"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
namespace serialctl
{
using Json = nlohmann::json;
class PowerService
{
  public:
    PowerService();
    ~PowerService();
    Json State();
    Json Submit(const Json &command);
    Json Action(const std::string &id);
    static Json UsbResources();
    static bool VisaAvailable();
    void Shutdown();

  private:
    struct Transport;
    void Run();
    void Execute(const std::string &id, const Json &command);
    void Poll();
    void Select(int channel);
    void Outputs(const Json &channels, bool enabled, Json &result);
    void StopTask();
    void Disconnect();
    void Command(const std::string &command);
    std::string Query(const std::string &command);
    double Number(const std::string &command);
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<std::string, Json>> queue_;
    std::map<std::string, Json> actions_;
    Json state_, task_;
    Json owned_ = Json::array();
    std::unique_ptr<Transport> transport_;
    SessionLogger log_, measurements_;
    std::thread thread_;
    bool stopping_ = false;
    std::uint64_t next_ = 1;
    std::uint64_t pollAt_ = 0, taskAt_ = 0;
};
} // namespace serialctl
