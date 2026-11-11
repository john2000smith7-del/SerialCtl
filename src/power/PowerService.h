#pragma once
#include "Connection.h"
#include "PowerKeywordRun.h"
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
    static bool ConnectionAvailable() { return testEnabled_.load(); }
    bool AuthorizeTest(const std::wstring& password);
    void DisableTest();
    static constexpr const char *ConnectionUnavailableMessage() {
        return u8"该功能暂未开放，请等待后续开放。";
    }
    Json State();
    Json Submit(const Json &command, const std::string &connectionId = {}, const std::string &expectedMode = {},
                std::shared_ptr<PowerCycleObserver> cycle = {});
    void LogKeywordEvent(const std::wstring &event) { log_.WriteStatus(event); }
    Json Action(const std::string &id);
    void CancelRemotePending(const std::string &source);
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
    friend struct AiDeviceTestAccess;
    bool testPauseRequested_ = false;
    bool testWorkerPaused_ = false;
    bool testPauseRecoveryOn_ = false, testRecoveryAtOn_ = false;
    void FailNextOutputReadbackForTest();
    void FailNextOutputOffWriteForTest();
#endif
    struct Transport;
    void Run();
    void Execute(const std::string &id, const Json &command, const std::shared_ptr<PowerCycleObserver> &cycle = {});
    void Poll();
    void ReadMode();
    void ChangeMode(const std::string &mode);
    void SetParameters(const Json &settings);
    void Extended(const Json &command, Json &action);
    void AdvanceTask();
    void CheckScpi();
    void Select(int channel);
    void Outputs(const Json &channels, bool enabled, Json &result);
    void StopTask(Json *offResults = nullptr, bool completed = false);
    void CancelKeywordCycle(const wchar_t *reason); // mutex_ held
    void Disconnect(bool preserveOutputs = false, Json *action = nullptr);
    void Command(const std::string &command);
    std::string Query(const std::string &command);
    double Number(const std::string &command);
    std::mutex mutex_;
    std::condition_variable wake_;
    struct Job {
        std::string id;
        Json command;
        std::shared_ptr<PowerCycleObserver> cycle;
    };
    std::deque<Job> queue_;
    std::weak_ptr<PowerCycleObserver> keywordCycle_;  // Submission and cancellation, mutex_ held.
    std::shared_ptr<PowerCycleObserver> activeCycle_; // Power worker only.
    std::map<std::string, Json> actions_;
    Json state_, task_;
    Json owned_ = Json::array();
    std::unique_ptr<Transport> transport_;
    SessionLogger log_, measurements_;
    std::thread thread_;
    bool stopping_ = false, lockRequested_ = false;
    static std::atomic<bool> testEnabled_;
    std::uint64_t next_ = 1;
    std::uint64_t pollAt_ = 0, taskAt_ = 0;
    unsigned pollInterval_ = 1000;
    std::uint64_t recoveryNotBefore_[3]{};
    std::atomic<std::uint64_t> recoveryCancelEpoch_{0};
    std::uint64_t taskRecoveryEpoch_ = 0;
};
} // namespace serialctl
