#pragma once
#include "AiProtocol.h"
#include "PuttyHostKey.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace serialctl::ai {
struct ServerSettings {
    std::wstring host, username, password;
    std::wstring workingDirectory = L"/home/linux/workspace";
    unsigned port = 22;
    bool rememberPassword = false;
    bool platformMode = false;
    std::wstring employeeNumber;
};
bool ValidateServer(const ServerSettings &, std::wstring &error);
ServerSettings LoadServerSettings();
bool ForgetSavedServerPassword(const ServerSettings &, std::wstring &error);
bool IsAuthenticationFailure(const std::wstring &message);
bool SaveServerSettings(const ServerSettings &, std::wstring &error);

// Owns SSH stdio only. No HWND, terminal model, device service or model API key.
// One bounded worker performs all network/process/JSON work off the UI thread.
class CodexClient {
  public:
    using Event = std::function<void(Json)>;
    ~CodexClient();
    bool Connect(ServerSettings settings, Event event, HostKeyConfirmation confirm, bool reverifyHostKey = false);
    void Disconnect();
    void Shutdown();
    bool Running() const {
        return running_;
    }
    bool Ready() const {
        return ready_;
    }
    bool Cancelled() const {
        return stop_;
    }
    bool Send(Json message);

  private:
    void Run(ServerSettings, HostKeyConfirmation, bool reverifyHostKey);
    void Emit(Json event);
    std::thread worker_;
    std::atomic<bool> stop_{false}, running_{false}, ready_{false};
    std::mutex mutex_;
    std::deque<std::string> writes_;
    size_t queuedBytes_ = 0;
    Event event_;
};
} // namespace serialctl::ai
