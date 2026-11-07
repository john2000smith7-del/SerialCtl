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

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring& str) {
    if (str.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring GetSettingsIniPath() {
    wchar_t modPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modPath, MAX_PATH);
    std::wstring path = modPath;
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) path = path.substr(0, pos + 1);
    return path + L"state.ini";
}
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
    dispatcher_.SetGuiExecutor([this](const std::string& method, const Json& params) -> Json {
        if (!IsGuiRunning()) {
            if (method.rfind("tftp.", 0) == 0) {
                return HandleOfflineTftp(method, params);
            }
            return {{"error", "GUI is not running and requested tool requires GUI"}};
        }
        if (method.rfind("tftp.", 0) == 0 && tftpServer_.IsListening()) {
            tftpServer_.Stop();
        }
        Json req = {{"method", method}, {"params", params}};
        std::string respStr;
        if (!SendIpcRequest(DefaultMcpPipeName, req.dump(), respStr, 5000)) {
            if (method.rfind("tftp.", 0) == 0) {
                return HandleOfflineTftp(method, params);
            }
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
    dispatcher_.SetServiceConfigurator([this](const Json& args) -> Json {
        uint16_t newPort = port_;
        std::string newBindIp = bindIp_;
        bool newAutoStart = IsUserAutoStartEnabled();

        if (args.contains("port")) {
            int p = args["port"].get<int>();
            if (p < 1 || p > 65535) return {{"error", "Invalid port: must be 1..65535"}};
            newPort = static_cast<uint16_t>(p);
        }
        if (args.contains("bindIp")) {
            std::string ip = args["bindIp"].get<std::string>();
            sockaddr_in sa{};
            if (ip != "0.0.0.0" && InetPtonA(AF_INET, ip.c_str(), &sa.sin_addr) <= 0) {
                return {{"error", "Invalid bindIp: must be valid IPv4 address"}};
            }
            newBindIp = ip;
        }
        if (args.contains("autoStart")) {
            newAutoStart = args["autoStart"].get<bool>();
            wchar_t modPath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, modPath, MAX_PATH);
            SetUserAutoStart(newAutoStart, modPath);
        }

        std::wstring iniPath = GetSettingsIniPath();
        WritePrivateProfileStringW(L"Mcp", L"Port", std::to_wstring(newPort).c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Mcp", L"BindIp", Utf8ToWide(newBindIp).c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Mcp", L"AutoStart", newAutoStart ? L"1" : L"0", iniPath.c_str());

        if ((newPort != port_ || newBindIp != bindIp_) && IsRunning()) {
            httpServer_.Stop();
            std::wstring err;
            auto reqHandler = [this](const std::string& ip, const std::string& body) {
                return dispatcher_.Dispatch(ip, body);
            };
            auto healthHandler = [this]() {
                return dispatcher_.CallTool("127.0.0.1", "app.health", Json::object()).dump();
            };
            if (!httpServer_.Start(newBindIp, newPort, reqHandler, healthHandler, err)) {
                httpServer_.Start(bindIp_, port_, reqHandler, healthHandler, err);
                return {{"error", "Failed to bind new address: " + WideToUtf8(err)}};
            }
            port_ = newPort;
            bindIp_ = newBindIp;
        } else {
            port_ = newPort;
            bindIp_ = newBindIp;
        }

        return {
            {"ok", true},
            {"port", port_},
            {"bindIp", bindIp_},
            {"autoStart", newAutoStart}
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
    LoadTftpConfig();
    return true;
}

void McpService::StopService() {
    if (!running_.exchange(false)) return;

    httpServer_.Stop();
    tftpServer_.Stop();

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

    DWORD selfSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &selfSession);

    // 2. Check for Session 0 / Interactive session requirement
    if (s_simulateSession0_ || selfSession == 0) {
        DWORD activeSession = WTSGetActiveConsoleSessionId();
        if (activeSession == 0xFFFFFFFF) {
            error = "LOGIN_REQUIRED: No active interactive user logon session found";
            return false;
        }

        HMODULE hWts = LoadLibraryW(L"wtsapi32.dll");
        HMODULE hUserEnv = LoadLibraryW(L"userenv.dll");

        typedef BOOL(WINAPI * PFN_WTSQueryUserToken)(ULONG SessionId, PHANDLE phToken);
        typedef BOOL(WINAPI * PFN_CreateEnvironmentBlock)(LPVOID * lpEnvironment, HANDLE hToken, BOOL bInherit);
        typedef BOOL(WINAPI * PFN_DestroyEnvironmentBlock)(LPVOID lpEnvironment);

        PFN_WTSQueryUserToken pWTSQueryUserToken = hWts ? (PFN_WTSQueryUserToken)GetProcAddress(hWts, "WTSQueryUserToken") : nullptr;
        PFN_CreateEnvironmentBlock pCreateEnvironmentBlock = hUserEnv ? (PFN_CreateEnvironmentBlock)GetProcAddress(hUserEnv, "CreateEnvironmentBlock") : nullptr;
        PFN_DestroyEnvironmentBlock pDestroyEnvironmentBlock = hUserEnv ? (PFN_DestroyEnvironmentBlock)GetProcAddress(hUserEnv, "DestroyEnvironmentBlock") : nullptr;

        HANDLE userToken = nullptr;
        if (!pWTSQueryUserToken || !pWTSQueryUserToken(activeSession, &userToken)) {
            if (hUserEnv) FreeLibrary(hUserEnv);
            if (hWts) FreeLibrary(hWts);
            error = "LOGIN_REQUIRED: Failed to query user token for session " + std::to_string(activeSession);
            return false;
        }

        HANDLE primaryToken = nullptr;
        if (!DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primaryToken)) {
            CloseHandle(userToken);
            if (hUserEnv) FreeLibrary(hUserEnv);
            if (hWts) FreeLibrary(hWts);
            error = "Failed to duplicate user token";
            return false;
        }
        CloseHandle(userToken);

        LPVOID pEnv = nullptr;
        if (pCreateEnvironmentBlock) {
            pCreateEnvironmentBlock(&pEnv, primaryToken, FALSE);
        }

        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

        STARTUPINFOW si{sizeof(si)};
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        PROCESS_INFORMATION pi{};

        DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT;
        BOOL launched = CreateProcessAsUserW(
            primaryToken,
            modulePath,
            nullptr,
            nullptr,
            nullptr,
            FALSE,
            creationFlags,
            pEnv,
            nullptr,
            &si,
            &pi);

        if (pEnv && pDestroyEnvironmentBlock) {
            pDestroyEnvironmentBlock(pEnv);
        }
        CloseHandle(primaryToken);
        if (hUserEnv) FreeLibrary(hUserEnv);
        if (hWts) FreeLibrary(hWts);

        if (!launched) {
            error = "CreateProcessAsUserW failed with error: " + std::to_string(GetLastError());
            return false;
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};

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
    }

    // Wait up to 5000ms for GUI IPC to become active
    for (int i = 0; i < 50; ++i) {
        Sleep(100);
        if (IsGuiRunning()) {
            return true;
        }
    }

    error = "GUI launched but IPC pipeline failed to become ready within timeout";
    return false;
}

Json McpService::HandleOfflineTftp(const std::string& method, const Json& params) {
    auto error = [](const std::string& msg) -> Json { return {{"error", msg}}; };

    if (method == "tftp.status") {
        Json res = {{"ok", true}};
        res["status"] = tftpServer_.State();
        res["listening"] = tftpServer_.IsListening();
        res["bindIp"] = tftpServer_.BindIp();
        res["port"] = tftpServer_.Port();
        res["serverIps"] = tftpServer_.LocalIps();
        res["publishedCount"] = tftpServer_.PublishedCount();
        res["activeTransfers"] = tftpServer_.ActiveTransferCount();
        res["guiOnline"] = false;
        return res;
    }
    if (method == "tftp.start") {
        std::string ip = params.value("bindIp", bindIp_);
        uint16_t p = static_cast<uint16_t>(params.value("port", 69));
        std::wstring err;
        if (!tftpServer_.Start(ip, p, err)) {
            return error(WideToUtf8(err));
        }
        PersistTftpConfig();
        return {
            {"ok", true},
            {"status", tftpServer_.State()},
            {"bindIp", tftpServer_.BindIp()},
            {"port", tftpServer_.Port()},
            {"serverIps", tftpServer_.LocalIps()}
        };
    }
    if (method == "tftp.stop") {
        tftpServer_.Stop();
        PersistTftpConfig();
        return {{"ok", true}, {"status", "stopped"}};
    }
    if (method == "tftp.publish_local") {
        std::string tftpName = params.value("tftpName", "");
        std::string localPath = params.value("localPath", "");
        bool overwrite = params.value("allowOverwrite", false);
        if (tftpName.empty() || localPath.empty()) return error("tftpName and localPath are required");
        auto pathW = Utf8ToWide(localPath);
        std::wstring err;
        if (!tftpServer_.PublishLocal(tftpName, pathW, overwrite, err)) {
            return error(WideToUtf8(err));
        }
        PersistTftpConfig();
        return {
            {"ok", true},
            {"tftpName", tftpName},
            {"localPath", localPath},
            {"status", "Ready"}
        };
    }
    if (method == "tftp.publish_remote") {
        return error("Remote streaming publish requires GUI with active SSH session");
    }
    if (method == "tftp.unpublish") {
        std::string tftpName = params.value("tftpName", "");
        if (tftpName.empty()) return error("tftpName is required");
        bool ok = tftpServer_.Unpublish(tftpName);
        PersistTftpConfig();
        return {{"ok", ok}, {"tftpName", tftpName}};
    }
    if (method == "tftp.list") {
        Json items = Json::array();
        for (const auto& item : tftpServer_.ListPublished()) {
            auto info = item.source ? item.source->Info() : TftpSourceInfo{};
            items.push_back({
                {"tftpName", item.tftpName},
                {"type", info.type},
                {"path", WideToUtf8(info.displayPath)},
                {"size", info.size},
                {"status", info.status}
            });
        }
        return {{"ok", true}, {"items", items}};
    }
    if (method == "tftp.transfers") {
        Json arr = Json::array();
        for (const auto& t : tftpServer_.ListTransfers()) {
            arr.push_back({
                {"transferId", t.transferId},
                {"clientIp", t.clientIp},
                {"clientPort", t.clientPort},
                {"tftpName", t.tftpName},
                {"phase", t.phase},
                {"sentBytes", t.sentBytes},
                {"ackedBytes", t.ackedBytes},
                {"totalBytes", t.totalBytes},
                {"retransmits", t.retransmits},
                {"speedBps", t.speedBps},
                {"error", WideToUtf8(t.error)}
            });
        }
        return {{"ok", true}, {"transfers", arr}};
    }
    if (method == "tftp.cancel_transfer") {
        uint64_t tid = params.value("transferId", 0ULL);
        bool ok = tftpServer_.CancelTransfer(tid);
        return {{"ok", ok}, {"transferId", tid}};
    }
    return error("Unknown TFTP tool");
}

void McpService::LoadTftpConfig() {
    std::wstring iniPath = GetSettingsIniPath();
    int enabled = GetPrivateProfileIntW(L"Tftp", L"Enabled", 0, iniPath.c_str());
    int port = GetPrivateProfileIntW(L"Tftp", L"Port", 69, iniPath.c_str());
    wchar_t ipBuf[64]{};
    GetPrivateProfileStringW(L"Tftp", L"BindIp", L"0.0.0.0", ipBuf, 64, iniPath.c_str());
    std::string ip = WideToUtf8(ipBuf);

    int localCount = GetPrivateProfileIntW(L"Tftp", L"LocalCount", -1, iniPath.c_str());
    if (localCount >= 0) {
        for (int i = 0; i < localCount; ++i) {
            std::wstring key = L"LocalItem_" + std::to_wstring(i);
            wchar_t itemBuf[1024]{};
            GetPrivateProfileStringW(L"Tftp", key.c_str(), L"", itemBuf, 1024, iniPath.c_str());
            std::wstring row(itemBuf);
            auto sep = row.find(L'|');
            if (sep != std::wstring::npos && sep > 0) {
                std::string tftpName = WideToUtf8(row.substr(0, sep));
                std::wstring localPath = row.substr(sep + 1);
                std::wstring err;
                tftpServer_.PublishLocal(tftpName, localPath, true, err);
            }
        }
    } else {
        int count = GetPrivateProfileIntW(L"Tftp", L"Count", 0, iniPath.c_str());
        for (int i = 0; i < count; ++i) {
            wchar_t nameKey[32], pathKey[32];
            swprintf_s(nameKey, L"Item%d_Name", i);
            swprintf_s(pathKey, L"Item%d_Path", i);
            wchar_t nameBuf[256]{}, pathBuf[MAX_PATH]{};
            GetPrivateProfileStringW(L"Tftp", nameKey, L"", nameBuf, 256, iniPath.c_str());
            GetPrivateProfileStringW(L"Tftp", pathKey, L"", pathBuf, MAX_PATH, iniPath.c_str());
            if (nameBuf[0] && pathBuf[0]) {
                std::wstring err;
                tftpServer_.PublishLocal(WideToUtf8(nameBuf), pathBuf, true, err);
            }
        }
    }

    if (enabled && !IsGuiRunning() && !tftpServer_.IsListening()) {
        std::wstring err;
        tftpServer_.Start(ip, static_cast<uint16_t>(port), err);
    }
}

void McpService::PersistTftpConfig() {
    std::wstring iniPath = GetSettingsIniPath();
    WritePrivateProfileStringW(L"Tftp", L"Enabled", tftpServer_.IsListening() ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Tftp", L"Port", std::to_wstring(tftpServer_.Port()).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Tftp", L"BindIp", Utf8ToWide(tftpServer_.BindIp()).c_str(), iniPath.c_str());

    auto published = tftpServer_.ListPublished();
    int localCount = 0;
    for (const auto& item : published) {
        if (item.source && item.source->Info().type == "local" && !item.source->Info().displayPath.empty()) {
            std::wstring key = L"LocalItem_" + std::to_wstring(localCount++);
            std::wstring val = Utf8ToWide(item.tftpName) + L"|" + item.source->Info().displayPath;
            WritePrivateProfileStringW(L"Tftp", key.c_str(), val.c_str(), iniPath.c_str());
        }
    }
    WritePrivateProfileStringW(L"Tftp", L"LocalCount", std::to_wstring(localCount).c_str(), iniPath.c_str());
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
