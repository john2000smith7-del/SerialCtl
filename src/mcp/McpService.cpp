#include "McpService.h"
#include <sddl.h>
#include <vector>

namespace serialctl::mcp {

namespace {
SERVICE_STATUS s_serviceStatus{};
SERVICE_STATUS_HANDLE s_serviceStatusHandle = nullptr;
HANDLE s_serviceStopEvent = nullptr;

const wchar_t* kAutoStartRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kAutoStartValueName = L"SerialCtlMcpAgent";
const wchar_t* kUserAgentStopEventName = L"Local\\SerialCtl.McpAgentStop.v1";
}

McpService& McpService::Instance() {
    static McpService s_instance;
    return s_instance;
}

McpService::McpService() {
    ConfigureDispatcher();
}

McpService::~McpService() {
    StopService();
}

void McpService::ConfigureDispatcher() {
    dispatcher_.SetGuiStatusChecker([this] { return IsGuiRunning(); });
    dispatcher_.SetGuiLauncher([this](bool focus, std::string& err) { return LaunchGui(focus, err); });
    dispatcher_.SetPowerContextProvider([](bool& testUnlocked, Json& selectedChannels) {
        Json req = {{"method", "internal.power_context"}, {"params", Json::object()}};
        std::string respStr;
        if (SendIpcRequest(DefaultMcpPipeName, req.dump(), respStr, 2000)) {
            try {
                Json resp = Json::parse(respStr);
                if (resp.is_object()) {
                    testUnlocked = resp.value("testUnlocked", false);
                    if (resp.contains("selectedChannels") && resp["selectedChannels"].is_array()) {
                        selectedChannels = resp["selectedChannels"];
                    }
                }
            } catch (...) {}
        }
    });
    dispatcher_.SetGuiExecutor([](const std::string& method, const Json& params) -> Json {
        Json req = {{"method", method}, {"params", params}};
        std::string respStr;
        if (!SendIpcRequest(DefaultMcpPipeName, req.dump(), respStr, 5000)) {
            return {{"error", "Failed to communicate with GUI via IPC"}};
        }
        try {
            return Json::parse(respStr);
        } catch (...) {
            return {{"error", "Failed to parse GUI response"}};
        }
    });
    dispatcher_.SetServiceStatusProvider([this] {
        return Json{
            {"serviceRunning", IsRunning() || IsAnyServiceProcessRunning()},
            {"autoStart", IsUserAutoStartEnabled() || IsScmServiceInstalled()},
            {"port", port_},
            {"bindIp", bindIp_}
        };
    });
}

bool McpService::InstallScmService(const std::wstring& exePath, uint16_t /*port*/, std::wstring& error) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            error = L"Administrator privileges required to install Windows Service.";
        } else {
            error = L"OpenSCManager failed with error: " + std::to_wstring(err);
        }
        return false;
    }

    std::wstring binaryPath = L"\"" + exePath + L"\" --mcp-service";

    SC_HANDLE service = CreateServiceW(
        scm,
        McpServiceName,
        McpServiceDisplayName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        binaryPath.c_str(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    if (!service) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_EXISTS) {
            error = L"Service is already installed.";
        } else {
            error = L"CreateService failed with error: " + std::to_wstring(err);
        }
        return false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}

bool McpService::UninstallScmService(std::wstring& error) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        error = L"OpenSCManager failed with error: " + std::to_wstring(GetLastError());
        return false;
    }

    SC_HANDLE service = OpenServiceW(scm, McpServiceName, SERVICE_STOP | DELETE);
    if (!service) {
        CloseServiceHandle(scm);
        error = L"Service not found or open failed: " + std::to_wstring(GetLastError());
        return false;
    }

    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);

    if (!DeleteService(service)) {
        DWORD err = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        error = L"DeleteService failed with error: " + std::to_wstring(err);
        return false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}

bool McpService::IsScmServiceInstalled() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE service = OpenServiceW(scm, McpServiceName, SERVICE_QUERY_STATUS);
    if (service) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return true;
    }

    CloseServiceHandle(scm);
    return false;
}

bool McpService::IsScmServiceRunning() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE service = OpenServiceW(scm, McpServiceName, SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytesNeeded = 0;
    bool isRunning = false;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp),
                             sizeof(ssp), &bytesNeeded)) {
        isRunning = (ssp.dwCurrentState == SERVICE_RUNNING);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return isRunning;
}

bool McpService::StartScmService(std::wstring& error) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error = L"OpenSCManager failed: " + std::to_wstring(GetLastError());
        return false;
    }

    SC_HANDLE service = OpenServiceW(scm, McpServiceName, SERVICE_START);
    if (!service) {
        CloseServiceHandle(scm);
        error = L"OpenService failed: " + std::to_wstring(GetLastError());
        return false;
    }

    if (!::StartServiceW(service, 0, nullptr)) {
        DWORD err = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_ALREADY_RUNNING) return true;
        error = L"StartService failed: " + std::to_wstring(err);
        return false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}

bool McpService::StopScmService(std::wstring& error) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error = L"OpenSCManager failed: " + std::to_wstring(GetLastError());
        return false;
    }

    SC_HANDLE service = OpenServiceW(scm, McpServiceName, SERVICE_STOP);
    if (!service) {
        CloseServiceHandle(scm);
        error = L"OpenService failed: " + std::to_wstring(GetLastError());
        return false;
    }

    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        DWORD err = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        error = L"ControlService STOP failed: " + std::to_wstring(err);
        return false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}

bool McpService::SetUserAutoStart(bool enable, const std::wstring& exePath) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAutoStartRunKey, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) {
        return false;
    }

    bool success = false;
    if (enable) {
        std::wstring command = L"\"" + exePath + L"\" --mcp-agent";
        DWORD size = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
        success = (RegSetValueExW(key, kAutoStartValueName, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(command.c_str()), size) == ERROR_SUCCESS);
    } else {
        RegDeleteValueW(key, kAutoStartValueName);
        success = true;
    }

    RegCloseKey(key);
    return success;
}

bool McpService::IsUserAutoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAutoStartRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD size = 0;
    bool exists = (RegQueryValueExW(key, kAutoStartValueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    RegCloseKey(key);
    return exists;
}

uint16_t McpService::LoadConfiguredPort() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring path(modulePath);
    size_t slash = path.find_last_of(L"\\/");
    std::wstring iniPath = (slash == std::wstring::npos ? L"" : path.substr(0, slash + 1)) + L"state.ini";
    UINT port = GetPrivateProfileIntW(L"Mcp", L"Port", 7080, iniPath.c_str());
    return (port > 0 && port <= 65535) ? static_cast<uint16_t>(port) : 7080;
}

bool McpService::IsAnyServiceProcessRunning() {
    if (IsScmServiceRunning()) return true;
    HANDLE mtx = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\SerialCtl.McpService.v1");
    if (!mtx) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD wait = WaitForSingleObject(mtx, 0);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        ReleaseMutex(mtx);
        CloseHandle(mtx);
        return false;
    }
    CloseHandle(mtx);
    return true;
}

bool McpService::SpawnUserAgentProcess() {
    if (IsAnyServiceProcessRunning()) return true;
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring cmdLine = L"\"" + std::wstring(modulePath) + L"\" --mcp-agent";
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            modulePath,
            mutableCmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | DETACHED_PROCESS,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void McpService::SignalUserAgentStop() {
    HANDLE stopEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, kUserAgentStopEventName);
    if (stopEvt) {
        SetEvent(stopEvt);
        CloseHandle(stopEvt);
    }
}

bool McpService::StartService(const std::string& bindIp, uint16_t port, std::wstring& error) {
    if (running_.load()) return true;

    WSADATA wsaData{};
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // Mutex Global\SerialCtl.McpService.v1
    PSECURITY_DESCRIPTOR pSd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)",
            SDDL_REVISION_1, &pSd, nullptr)) {
        sa.lpSecurityDescriptor = pSd;
    }

    serviceMutex_ = CreateMutexW(&sa, TRUE, L"Global\\SerialCtl.McpService.v1");
    const DWORD mutexErr = GetLastError();
    if (pSd) LocalFree(pSd);

    if (serviceMutex_ && mutexErr == ERROR_ALREADY_EXISTS) {
        DWORD wait = WaitForSingleObject(serviceMutex_, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(serviceMutex_);
            serviceMutex_ = nullptr;
            error = L"Another SerialCtl MCP service instance is already running.";
            return false;
        }
    }

    bindIp_ = bindIp.empty() ? "0.0.0.0" : bindIp;
    port_ = port ? port : LoadConfiguredPort();

    ConfigureDispatcher();

    auto reqHandler = [this](const std::string& clientIp, const std::string& body) {
        return dispatcher_.Dispatch(clientIp, body);
    };

    auto healthHandler = [this]() {
        return dispatcher_.CallTool("127.0.0.1", "app.health", Json::object()).dump();
    };

    if (!httpServer_.Start(bindIp_, port_, reqHandler, healthHandler, error)) {
        if (serviceMutex_) {
            ReleaseMutex(serviceMutex_);
            CloseHandle(serviceMutex_);
            serviceMutex_ = nullptr;
        }
        return false;
    }

    running_.store(true);
    return true;
}

void McpService::StopService() {
    if (!running_.exchange(false)) return;

    httpServer_.Stop();

    if (serviceMutex_) {
        ReleaseMutex(serviceMutex_);
        CloseHandle(serviceMutex_);
        serviceMutex_ = nullptr;
    }
}

bool McpService::IsRunning() const {
    return running_.load();
}

bool McpService::IsGuiRunning() const {
    // Check if the GUI IPC pipe is connectable
    if (WaitNamedPipeW(DefaultMcpPipeName, 0)) {
        return true;
    }
    DWORD err = GetLastError();
    if (err == ERROR_PIPE_BUSY || err == ERROR_ACCESS_DENIED) {
        return true;
    }
    return false;
}

bool McpService::LaunchGui(bool /*forceFocus*/, std::string& error) {
    std::lock_guard<std::mutex> lock(launchMutex_);

    // 1. If GUI is already running, activate it
    if (IsGuiRunning()) {
        std::string resp;
        SendIpcRequest(DefaultMcpPipeName, "{\"method\":\"app.activate\"}", resp, 1000);
        return true;
    }

    // 2. Check for Session 0 / Interactive session requirement
    if (s_simulateSession0_) {
        error = "LOGIN_REQUIRED: Interactive logon session required (Session 0 detected)";
        return false;
    }

    DWORD activeSession = WTSGetActiveConsoleSessionId();
    if (activeSession == 0xFFFFFFFF) {
        error = "LOGIN_REQUIRED: No active interactive user logon session found";
        return false;
    }

    // 3. Launch single serialctl.exe instance
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};

    // Launch without arguments -> starts normal GUI
    if (!CreateProcessW(
            modulePath,
            nullptr,
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        error = "CreateProcessW failed with error: " + std::to_string(GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Wait up to 3000ms for GUI IPC to become active
    for (int i = 0; i < 30; ++i) {
        Sleep(100);
        if (IsGuiRunning()) {
            return true;
        }
    }

    // Even if IPC didn't respond yet, process was launched
    return true;
}

int McpService::RunAsScmService() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(McpServiceName), ServiceMain},
        {nullptr, nullptr}
    };
    StartServiceCtrlDispatcherW(table);
    return 0;
}

void WINAPI McpService::ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    s_serviceStatusHandle = RegisterServiceCtrlHandlerExW(
        McpServiceName, ServiceCtrlHandler, nullptr);
    if (!s_serviceStatusHandle) return;

    s_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    s_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);

    s_serviceStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::wstring error;
    if (!Instance().StartService("0.0.0.0", LoadConfiguredPort(), error)) {
        s_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        s_serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);
        return;
    }

    s_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);

    WaitForSingleObject(s_serviceStopEvent, INFINITE);

    Instance().StopService();
    CloseHandle(s_serviceStopEvent);

    s_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);
}

DWORD WINAPI McpService::ServiceCtrlHandler(DWORD ctrl, DWORD /*eventType*/, LPVOID /*eventData*/, LPVOID /*context*/) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        if (s_serviceStopEvent) {
            SetEvent(s_serviceStopEvent);
        }
        s_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);
        return NO_ERROR;
    default:
        break;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

int McpService::RunAsUserAgent() {
    HANDLE stopEvt = CreateEventW(nullptr, TRUE, FALSE, kUserAgentStopEventName);
    if (stopEvt) ResetEvent(stopEvt);

    std::wstring error;
    if (!Instance().StartService("0.0.0.0", LoadConfiguredPort(), error)) {
        if (stopEvt) CloseHandle(stopEvt);
        return 1;
    }

    // Run message/event loop until signaled to stop or WM_QUIT
    MSG msg{};
    bool quit = false;
    while (!quit) {
        if (stopEvt) {
            DWORD wait = MsgWaitForMultipleObjects(1, &stopEvt, FALSE, INFINITE, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) {
                break;
            }
        } else {
            if (!GetMessageW(&msg, nullptr, 0, 0)) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    Instance().StopService();
    if (stopEvt) CloseHandle(stopEvt);
    return 0;
}

} // namespace serialctl::mcp
