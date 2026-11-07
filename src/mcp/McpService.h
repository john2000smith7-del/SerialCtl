#pragma once
#include <windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include "McpHttpServer.h"
#include "McpDispatcher.h"
#include "McpIpc.h"
#include "TftpServer.h"

namespace serialctl::mcp {

constexpr const wchar_t* McpServiceName = L"SerialCtlMcpService";
constexpr const wchar_t* McpServiceDisplayName = L"SerialCtl Resident MCP Service";

class McpService {
public:
    static McpService& Instance();

    // SCM Service management
    static bool InstallScmService(const std::wstring& exePath, uint16_t port, std::wstring& error);
    static bool UninstallScmService(std::wstring& error);
    static bool IsScmServiceInstalled();
    static bool IsScmServiceRunning();
    static bool StartScmService(std::wstring& error);
    static bool StopScmService(std::wstring& error);

    // HKCU Run auto-start management
    static bool SetUserAutoStart(bool enable, const std::wstring& exePath);
    static bool IsUserAutoStartEnabled();

    // Background User Agent process management
    static bool SpawnUserAgentProcess();
    static void SignalUserAgentStop();
    static bool IsAnyServiceProcessRunning();
    static uint16_t LoadConfiguredPort();

    // SCM Service entry points
    static int RunAsScmService();
    static int RunAsUserAgent();

    // Main service runner
    void ConfigureDispatcher();
    bool StartService(const std::string& bindIp, uint16_t port, std::wstring& error);
    void StopService();
    bool IsRunning() const;

    // Launch single GUI instance idempotently
    bool LaunchGui(bool forceFocus, std::string& error);

    // Check if GUI is running via IPC pipe or mutex
    bool IsGuiRunning() const;

    McpDispatcher& Dispatcher() { return dispatcher_; }
    McpHttpServer& HttpServer() { return httpServer_; }
    TftpServer& Tftp() { return tftpServer_; }

    uint16_t Port() const { return port_; }
    std::string BindIp() const { return bindIp_; }

    // Test support
    static void SetSimulateSession0ForTest(bool sim) { s_simulateSession0_ = sim; }

private:
    McpService();
    ~McpService();

    static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI ServiceCtrlHandler(DWORD ctrl, DWORD eventType, LPVOID eventData, LPVOID context);

    Json HandleOfflineTftp(const std::string& method, const Json& params);
    void LoadTftpConfig();
    void PersistTftpConfig();

    std::string bindIp_ = "0.0.0.0";
    uint16_t port_ = 7080;
    std::atomic<bool> running_{false};
    HANDLE serviceMutex_ = nullptr;
    std::mutex launchMutex_;

    McpDispatcher dispatcher_;
    McpHttpServer httpServer_;
    TftpServer tftpServer_;

    static inline bool s_simulateSession0_ = false;
};

} // namespace serialctl::mcp
