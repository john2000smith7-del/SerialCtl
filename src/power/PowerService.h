#pragma once
#include "Connection.h"
#include "SessionLogger.h"
#include "json.hpp"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
namespace serialctl {
using Json = nlohmann::json;
class PowerService {
  public:
    PowerService();
    ~PowerService();
    static constexpr bool ConnectionAvailable() { return false; }
    static constexpr const char *ConnectionUnavailableMessage() {
        return u8"该功能暂未开放，请等待后续开放。";
    }
    Json State();
    Json Submit(const Json &command);
    Json Action(const std::string &id);
    static Json UsbResources();
    static bool VisaAvailable();
    void RequestShutdown();
    void Shutdown();
    bool ExportLog(bool csv, const std::wstring &path, std::wstring &error);
    SessionLogger::ExportPoint CaptureLogExportPoint(bool csv);
    bool ExportLog(bool csv, const std::wstring &path, SessionLogger::ExportPoint point,
                   std::wstring &error, const std::atomic<bool> *cancel,
                   const SessionLogger::ExportProgress &progress);

  private:
#ifdef SERIALCTL_POWER_TESTING
    friend struct PowerServiceTestAccess;
    bool testPauseRequested_ = false;
    bool testWorkerPaused_ = false;
    bool testPauseRecoveryOn_ = false, testRecoveryAtOn_ = false;
    void FailNextOutputReadbackForTest();
#endif
    struct Transport;
    void Run();
    void Execute(const std::string &id, const Json &command);
    void Poll();
    void ReadMode();
    void ChangeMode(const std::string &mode);
    void SetParameters(const Json &settings);
    void Extended(const Json &command, Json &action);
    void AdvanceTask();
    void CheckScpi();
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
    unsigned pollInterval_ = 1000;
    std::uint64_t recoveryNotBefore_[3]{};
    std::atomic<std::uint64_t> recoveryCancelEpoch_{0};
    std::uint64_t taskRecoveryEpoch_ = 0;
};
} // namespace serialctl
