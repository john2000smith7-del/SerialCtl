#pragma once
#include "AiProtocol.h"
#include "PuttyHostKey.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace serialctl::ai {
enum class AiProvider {
    Codex = 0,
    Antigravity = 1
};
inline const wchar_t *AiProviderLabel(AiProvider p) {
    return p == AiProvider::Antigravity ? L"Antigravity" : L"Codex";
}
struct ServerSettings {
    std::wstring host, username, password;
    std::wstring workingDirectory = L"/home/linux/workspace";
    unsigned port = 22;
    bool rememberPassword = false;
    bool platformMode = false;
    std::wstring employeeNumber;
    AiProvider provider = AiProvider::Codex;
    bool localMode = false;
    unsigned mcpPort = 7080;
    unsigned remoteMcpPort = 0;
};
struct WorkspaceSettings {
    bool isLocal = false;
    bool localReadOnly = false;
    std::wstring localDirectory;
    std::wstring remoteDirectory = L"/home/linux/workspace";
    std::string remoteSshSessionId;
};
bool ValidateServer(const ServerSettings &, std::wstring &error);
ServerSettings LoadServerSettings();
std::wstring WorkspaceForServer(const ServerSettings&);
WorkspaceSettings LoadWorkspaceSettings(const ServerSettings *activeServer = nullptr);
bool SaveWorkspaceSettings(const WorkspaceSettings &, const ServerSettings *activeServer, std::wstring &error);
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
