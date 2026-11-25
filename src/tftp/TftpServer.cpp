#include "TftpServer.h"
#include "Win32Helpers.h"
#include "resource.h"
#include <iphlpapi.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace serialctl {
namespace {

std::atomic<uint64_t> g_tftpInstanceSeq{1};

std::wstring ToLowerCopy(const wchar_t* text) {
    if (!text) return {};
    std::wstring out(text);
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return out;
}

bool ContainsVirtualAdapterToken(const std::wstring& lowerText) {
    if (lowerText.empty()) return false;
    static constexpr const wchar_t* kVirtualTokens[] = {
        L"virtual", L"hyper-v", L"vethernet", L"vmware", L"virtualbox",
        L"wsl", L"docker", L"tunnel", L"tailscale", L"wireguard",
        L"zerotier", L"wintun", L"tap-", L"tap ", L"clash",
        L"mihomo", L"sing-box", L"npcap", L"bluetooth", L"loopback",
        L"teredo", L"isatap", L"vpn"
    };
    for (const wchar_t* token : kVirtualTokens) {
        if (lowerText.find(token) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

int ScoreIpv4Address(uint32_t hostOrderIp, bool isVirtual, bool isPhysicalEthernet,
                     bool isPhysicalWifi, bool hasGateway) {
    const bool isApipa = (hostOrderIp & 0xFFFF0000u) == 0xA9FE0000u;
    const bool isBenchmarkProxy = (hostOrderIp & 0xFFFE0000u) == 0xC6120000u;
    const bool isCgnat = (hostOrderIp & 0xFFC00000u) == 0x64400000u;
    const bool isRfc1918 = (hostOrderIp & 0xFF000000u) == 0x0A000000u ||
                           (hostOrderIp & 0xFFF00000u) == 0xAC100000u ||
                           (hostOrderIp & 0xFFFF0000u) == 0xC0A80000u;

    int score = 0;
    if (isPhysicalEthernet) score += 420;
    else if (isPhysicalWifi) score += 410;
    if (hasGateway && !isVirtual && !isBenchmarkProxy) score += 150;
    if (isRfc1918) score += 80;
    if (isVirtual) score -= 300;
    if (isBenchmarkProxy) score -= 400;
    if (isCgnat) score -= 200;
    if (isApipa) score -= 500;
    return score;
}

std::string WideToAnsi(const std::wstring& text, bool* usedDefaultChar = nullptr) {
    if (usedDefaultChar) *usedDefaultChar = false;
    if (text.empty()) return {};
    BOOL usedDefault = FALSE;
    const int needed = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr, &usedDefault);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(),
                        static_cast<int>(text.size()), out.data(), needed, nullptr, &usedDefault);
    if (usedDefaultChar) *usedDefaultChar = (usedDefault != FALSE);
    return out;
}

std::wstring ToAnsiCompatiblePath(const std::wstring& path) {
    if (path.empty()) return path;
    bool usedDefault = false;
    const std::string encoded = WideToAnsi(path, &usedDefault);
    if (!encoded.empty() && !usedDefault) {
        return path;
    }
    const DWORD needed = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (needed > 0) {
        std::wstring shortPath(needed, L'\0');
        const DWORD copied = GetShortPathNameW(path.c_str(), shortPath.data(), needed);
        if (copied > 0 && copied < needed) {
            shortPath.resize(copied);
            return shortPath;
        }
    }
    return path;
}

bool WriteBytesToFile(const std::wstring& path, const void* data, size_t size, std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法写入文件 " + path + L" (" + Win32ErrorMessage(GetLastError()) + L")";
        return false;
    }
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, ptr, chunk, &written, nullptr) || written != chunk) {
            const DWORD code = GetLastError();
            CloseHandle(file);
            error = L"写入文件失败 " + path + L" (" + Win32ErrorMessage(code) + L")";
            return false;
        }
        ptr += chunk;
        remaining -= chunk;
    }
    CloseHandle(file);
    return true;
}

bool ExtractTftp32Binary(const std::wstring& destPath, std::wstring& error) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_TFTP32_EXE), RT_RCDATA);
    if (!res) {
        error = L"程序缺少内置 tftp32.exe 资源";
        return false;
    }
    const DWORD size = SizeofResource(module, res);
    const HGLOBAL loaded = LoadResource(module, res);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || size == 0) {
        error = L"无法加载内置 tftp32.exe 资源";
        return false;
    }
    return WriteBytesToFile(destPath, bytes, size, error);
}

bool ProbeUdpBind(const sockaddr_in& addr, int& outWsaError) {
    outWsaError = 0;
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET) {
        outWsaError = WSAGetLastError();
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(probe, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    const int rc = bind(probe, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR) {
        outWsaError = WSAGetLastError();
        closesocket(probe);
        return false;
    }
    closesocket(probe);
    return true;
}

struct HiddenDesktopHandle {
    HDESK desk = nullptr;
    std::wstring name;
};

HiddenDesktopHandle CreateHiddenTftpDesktop(uint64_t seq) {
    HiddenDesktopHandle result;
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return result;
    using PFN_CreateDesktopW = HDESK(WINAPI*)(LPCWSTR, LPCWSTR, DEVMODEW*, DWORD, ACCESS_MASK, LPSECURITY_ATTRIBUTES);
    const auto pCreateDesktopW =
        reinterpret_cast<PFN_CreateDesktopW>(GetProcAddress(user32, "CreateDesktopW"));
    if (pCreateDesktopW) {
        result.name = L"SerialCtlTftpBg_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(seq);
        constexpr ACCESS_MASK kDesktopAccess =
            DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL |
            DESKTOP_JOURNALRECORD | DESKTOP_JOURNALPLAYBACK | DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS |
            DESKTOP_SWITCHDESKTOP;
        result.desk = pCreateDesktopW(result.name.c_str(), nullptr, nullptr, 0, kDesktopAccess, nullptr);
        if (!result.desk) {
            result.name.clear();
        }
    }
    FreeLibrary(user32);
    return result;
}

void CloseHiddenTftpDesktop(HDESK desk) {
    if (!desk) return;
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return;
    using PFN_CloseDesktop = BOOL(WINAPI*)(HDESK);
    const auto pCloseDesktop = reinterpret_cast<PFN_CloseDesktop>(GetProcAddress(user32, "CloseDesktop"));
    if (pCloseDesktop) {
        pCloseDesktop(desk);
    }
    FreeLibrary(user32);
}

} // namespace

TftpServer::TftpServer() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        wsaInitialized_ = true;
    }
}

TftpServer::~TftpServer() {
    Stop();
    if (wsaInitialized_) {
        WSACleanup();
        wsaInitialized_ = false;
    }
}

std::vector<std::string> TftpServer::EnumerateLocalIps() {
    struct IpCandidate {
        std::string ip;
        int score = 0;
    };
    std::vector<IpCandidate> candidates;
    std::map<std::string, size_t> indexByIp;
    auto addCandidate = [&](const char* ipStr, int score) {
        if (!ipStr || !*ipStr) return;
        const std::string key(ipStr);
        auto it = indexByIp.find(key);
        if (it == indexByIp.end()) {
            indexByIp[key] = candidates.size();
            candidates.push_back({key, score});
        } else if (score > candidates[it->second].score) {
            candidates[it->second].score = score;
        }
    };

    HMODULE iphlp = LoadLibraryW(L"iphlpapi.dll");
    if (iphlp) {
        using PFN_GetAdaptersAddresses = ULONG(WINAPI*)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
        const auto pGetAdaptersAddresses =
            reinterpret_cast<PFN_GetAdaptersAddresses>(GetProcAddress(iphlp, "GetAdaptersAddresses"));
        if (pGetAdaptersAddresses) {
            constexpr ULONG kFlags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                     GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
            ULONG bufLen = 16384;
            std::vector<uint8_t> buffer(bufLen);
            ULONG ret = ERROR_BUFFER_OVERFLOW;
            for (int attempt = 0; attempt < 3; ++attempt) {
                buffer.resize(bufLen);
                auto* adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                ret = pGetAdaptersAddresses(AF_INET, kFlags, nullptr, adapters, &bufLen);
                if (ret != ERROR_BUFFER_OVERFLOW) break;
            }
            if (ret == ERROR_SUCCESS) {
                for (PIP_ADAPTER_ADDRESSES adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                     adapter != nullptr;
                     adapter = adapter->Next) {
                    if (adapter->OperStatus != IfOperStatusUp) continue;
                    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

                    const std::wstring lowerDesc = ToLowerCopy(adapter->Description);
                    const std::wstring lowerFriendly = ToLowerCopy(adapter->FriendlyName);
                    const bool isVirtual = (adapter->IfType == IF_TYPE_TUNNEL || adapter->IfType == 53) ||
                                           ContainsVirtualAdapterToken(lowerDesc) ||
                                           ContainsVirtualAdapterToken(lowerFriendly);

                    bool hasGateway = false;
                    for (PIP_ADAPTER_GATEWAY_ADDRESS gw = adapter->FirstGatewayAddress; gw != nullptr; gw = gw->Next) {
                        if (gw->Address.lpSockaddr && gw->Address.lpSockaddr->sa_family == AF_INET) {
                            const auto* gsin = reinterpret_cast<const sockaddr_in*>(gw->Address.lpSockaddr);
                            if (gsin->sin_addr.s_addr != 0) {
                                hasGateway = true;
                                break;
                            }
                        }
                    }

                    const bool isPhysicalEthernet = (adapter->IfType == IF_TYPE_ETHERNET_CSMACD) && !isVirtual;
                    const bool isPhysicalWifi = (adapter->IfType == IF_TYPE_IEEE80211) && !isVirtual;

                    for (PIP_ADAPTER_UNICAST_ADDRESS ua = adapter->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                        if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                        const auto* sin = reinterpret_cast<const sockaddr_in*>(ua->Address.lpSockaddr);
                        const uint32_t hostOrderIp = ntohl(sin->sin_addr.s_addr);
                        if (hostOrderIp == 0 || (hostOrderIp & 0xFF000000u) == 0x7F000000u) continue;
                        char ipBuf[INET_ADDRSTRLEN]{};
                        if (!inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf))) continue;
                        const int score = ScoreIpv4Address(hostOrderIp, isVirtual, isPhysicalEthernet,
                                                           isPhysicalWifi, hasGateway);
                        addCandidate(ipBuf, score);
                    }
                }
            }
        }
        FreeLibrary(iphlp);
    }

    if (candidates.empty()) {
        char hostname[256] = {};
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
                for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
                    if (p->ai_family == AF_INET && p->ai_addr) {
                        const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
                        const uint32_t hostOrderIp = ntohl(sin->sin_addr.s_addr);
                        if (hostOrderIp == 0 || (hostOrderIp & 0xFF000000u) == 0x7F000000u) continue;
                        char ip[INET_ADDRSTRLEN] = {};
                        if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) {
                            const int score = ScoreIpv4Address(hostOrderIp, false, false, false, false);
                            addCandidate(ip, score);
                        }
                    }
                }
                freeaddrinfo(res);
            }
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const IpCandidate& a, const IpCandidate& b) { return a.score > b.score; });

    std::vector<std::string> ips;
    ips.reserve(candidates.size() + 1);
    for (const auto& candidate : candidates) {
        ips.push_back(candidate.ip);
    }
    if (ips.empty()) {
        ips.push_back("127.0.0.1");
    }
    return ips;
}

std::string TftpServer::DefaultLocalIp() {
    const auto ips = EnumerateLocalIps();
    return ips.empty() ? std::string("127.0.0.1") : ips.front();
}

std::vector<std::string> TftpServer::LocalIps() const {
    return EnumerateLocalIps();
}

std::string TftpServer::DefaultIp() const {
    return DefaultLocalIp();
}

bool TftpServer::IsProcessAliveLocked() const {
    if (!running_.load() || !processHandle_) return false;
    return WaitForSingleObject(processHandle_, 0) == WAIT_TIMEOUT;
}

bool TftpServer::EnsureStagingDirLocked(std::wstring& error) {
    namespace fs = std::filesystem;
    if (stagingDir_.empty()) {
        wchar_t tempPath[MAX_PATH]{};
        const DWORD n = GetTempPathW(MAX_PATH, tempPath);
        if (n == 0 || n >= MAX_PATH) {
            error = L"无法获取系统临时目录";
            return false;
        }
        const uint64_t seq = g_tftpInstanceSeq.fetch_add(1);
        stagingDir_ = (fs::path(std::wstring(tempPath, n)) /
            (L"serialctl_tftp32_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(seq))).native();
    }
    std::error_code ec;
    fs::create_directories(fs::path(stagingDir_) / L"published_root", ec);
    if (ec) {
        error = L"无法创建 tftp32 运行目录";
        return false;
    }
    stagingDir_ = ToAnsiCompatiblePath(stagingDir_);
    return true;
}

bool TftpServer::SyncPublishedFilesLocked(std::wstring& error) {
    namespace fs = std::filesystem;
    if (stagingDir_.empty()) return true;
    const fs::path pubRoot = fs::path(stagingDir_) / L"published_root";
    std::error_code ec;
    fs::create_directories(pubRoot, ec);
    if (ec) {
        error = L"无法准备 TFTP 发布目录";
        return false;
    }
    for (const auto& entry : fs::directory_iterator(pubRoot, ec)) {
        fs::remove_all(entry.path(), ec);
    }
    std::map<std::string, std::shared_ptr<LocalTftpFileSource>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = publishedFiles_;
    }
    for (const auto& [name, src] : snapshot) {
        if (!src) continue;
        const std::wstring wname = MultiByteToWide(reinterpret_cast<const uint8_t*>(name.data()), name.size(), CP_UTF8);
        if (wname.empty()) continue;
        const fs::path dst = pubRoot / wname;
        if (!CopyFileW(src->Path().c_str(), dst.c_str(), FALSE)) {
            error = L"无法同步发布文件到 TFTP 根目录: " + wname;
            return false;
        }
    }
    return true;
}

bool TftpServer::WriteIniLocked(const std::wstring& baseDir, const std::string& bindIp, uint16_t port, std::wstring& error) {
    namespace fs = std::filesystem;
    const std::wstring safeBaseDir = ToAnsiCompatiblePath(baseDir);
    bool usedDefault = false;
    const std::string ansiBaseDir = WideToAnsi(safeBaseDir, &usedDefault);
    if (ansiBaseDir.empty() || usedDefault) {
        error = L"所选目录路径包含系统代码页无法识别的字符，且无可用的短路径";
        return false;
    }
    const std::string iniLocalIp = (bindIp == "0.0.0.0") ? std::string{} : bindIp;

    std::ostringstream ini;
    ini << "[DHCP]\r\n"
        << "Lease_NumLeases=0\r\n"
        << "[TFTPD32]\r\n"
        << "BaseDirectory=" << ansiBaseDir << "\r\n"
        << "TftpPort=" << port << "\r\n"
        << "Hide=0\r\n"
        << "WinSize=0\r\n"
        << "Negociate=1\r\n"
        << "PXECompatibility=0\r\n"
        << "DirText=0\r\n"
        << "ShowProgressBar=0\r\n"
        << "Timeout=2\r\n"
        << "MaxRetransmit=5\r\n"
        << "SecurityLevel=1\r\n"
        << "UnixStrings=1\r\n"
        << "Beep=0\r\n"
        << "VirtualRoot=0\r\n"
        << "MD5=0\r\n"
        << "LocalIP=" << iniLocalIp << "\r\n"
        << "Services=1\r\n"
        << "TftpLogFile=\r\n"
        << "SaveSyslogFile=\r\n"
        << "PipeSyslogMsg=0\r\n"
        << "LowestUDPPort=0\r\n"
        << "HighestUDPPort=0\r\n"
        << "MulticastPort=0\r\n"
        << "MulticastAddress=\r\n"
        << "PersistantLeases=1\r\n"
        << "DHCP Ping=1\r\n"
        << "DHCP LocalIP=\r\n"
        << "Max Simultaneous Transfers=100\r\n"
        << "UseEventLog=0\r\n"
        << "Console Password=tftpd32\r\n"
        << "Support for port Option=0\r\n"
        << "Keep transfer Gui=0\r\n"
        << "Ignore ack for last TFTP packet=0\r\n"
        << "Enable IPv6=0\r\n";

    const std::string bytes = ini.str();
    const std::wstring iniPath = (fs::path(stagingDir_) / L"tftpd32.ini").native();
    return WriteBytesToFile(iniPath, bytes.data(), bytes.size(), error);
}

bool TftpServer::Start(const std::string& bindIp, std::wstring& error) {
    return Start(bindIp, 69, error);
}

bool TftpServer::Start(const std::string& bindIp, uint16_t port, std::wstring& error) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    return StartLocked(bindIp, port, error);
}

bool TftpServer::StartLocked(const std::string& bindIp, uint16_t port, std::wstring& error) {
    namespace fs = std::filesystem;
    if (IsProcessAliveLocked()) {
        error = L"TFTP 服务已经在运行";
        return false;
    }
    if (processHandle_ || jobHandle_) {
        StopLocked();
    }

    const std::string requestedIp = bindIp.empty() ? DefaultLocalIp() : bindIp;
    const uint16_t effectivePort = port ? port : 69;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (InetPtonA(AF_INET, requestedIp.c_str(), &addr.sin_addr) != 1) {
        error = L"无效的 TFTP 监听 IPv4 地址。";
        return false;
    }
    addr.sin_port = htons(effectivePort);

    int bindErr = 0;
    if (!ProbeUdpBind(addr, bindErr)) {
        if (bindErr == WSAEADDRINUSE) {
            error = L"端口 " + std::to_wstring(effectivePort) + L" 已被占用。请关闭冲突服务后重试。";
        } else if (bindErr == WSAEACCES) {
            error = L"端口 " + std::to_wstring(effectivePort) + L" 权限不足（标准 69 端口可能需要管理员权限，或已被系统限制占用）。";
        } else {
            error = L"绑定 UDP 端口失败: 错误代码 " + std::to_wstring(bindErr);
        }
        return false;
    }

    if (!EnsureStagingDirLocked(error)) {
        return false;
    }

    const fs::path exePath = fs::path(stagingDir_) / L"tftp32.exe";
    if (!fs::exists(exePath)) {
        if (!ExtractTftp32Binary(exePath.native(), error)) {
            return false;
        }
    }

    TftpRootConfig rootSnapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rootSnapshot = rootConfig_;
    }

    std::wstring effectiveBaseDir;
    if (rootSnapshot.mode == TftpRootMode::Local && !rootSnapshot.localDirectory.empty()) {
        std::wstring normalized;
        if (!ValidateLocalTftpDirectory(rootSnapshot.localDirectory, normalized, error)) {
            return false;
        }
        effectiveBaseDir = normalized;
    } else {
        if (!SyncPublishedFilesLocked(error)) {
            return false;
        }
        effectiveBaseDir = (fs::path(stagingDir_) / L"published_root").native();
    }

    if (!WriteIniLocked(effectiveBaseDir, requestedIp, effectivePort, error)) {
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        error = L"创建 tftp32 进程托管作业失败: " + Win32ErrorMessage(GetLastError());
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit))) {
        const DWORD code = GetLastError();
        CloseHandle(job);
        error = L"配置 tftp32 托管策略失败: " + Win32ErrorMessage(code);
        return false;
    }

    HiddenDesktopHandle hiddenDesk = CreateHiddenTftpDesktop(g_tftpInstanceSeq.fetch_add(1));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = hiddenDesk.desk ? hiddenDesk.name.data() : nullptr;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring cmdLine = QuoteCommandLineArgument(exePath.native());
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exePath.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, stagingDir_.c_str(), &si, &pi)) {
        const DWORD code = GetLastError();
        CloseHandle(job);
        CloseHiddenTftpDesktop(hiddenDesk.desk);
        error = L"无法启动 tftp32.exe: " + Win32ErrorMessage(code);
        return false;
    }
    CloseHandle(pi.hThread);

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        // Non-fatal if parent is inside a nested job on older Win7 without breakaway,
        // processHandle_ termination in Stop() / ~TftpServer() still guarantees cleanup.
    }

    HANDLE procHandle = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobHandle_ = job;
        processHandle_ = pi.hProcess;
        hiddenDesktop_ = hiddenDesk.desk;
        processId_ = pi.dwProcessId;
        procHandle = processHandle_;
    }

    // Wait up to 2500 ms for tftp32.exe to bind the configured UDP port
    bool boundReady = false;
    const ULONGLONG deadline = GetTickCount64() + 2500ULL;
    while (GetTickCount64() < deadline) {
        if (WaitForSingleObject(procHandle, 0) != WAIT_TIMEOUT) {
            StopLocked();
            error = L"tftp32.exe 启动后意外退出";
            return false;
        }
        int probeErr = 0;
        if (!ProbeUdpBind(addr, probeErr) && (probeErr == WSAEADDRINUSE || probeErr == WSAEACCES)) {
            boundReady = true;
            break;
        }
        Sleep(25);
    }

    if (!boundReady) {
        StopLocked();
        error = L"tftp32.exe 启动超时或未能监听端口 " + std::to_wstring(effectivePort);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bindIp_ = requestedIp;
        port_ = effectivePort;
        lastError_.clear();
        running_.store(true);
    }
    return true;
}

void TftpServer::Stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    StopLocked();
}

void TftpServer::StopLocked() {
    HANDLE procToClose = nullptr;
    HANDLE jobToClose = nullptr;
    HDESK deskToClose = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.store(false);
        procToClose = processHandle_;
        processHandle_ = nullptr;
        jobToClose = jobHandle_;
        jobHandle_ = nullptr;
        deskToClose = hiddenDesktop_;
        hiddenDesktop_ = nullptr;
        processId_ = 0;
    }
    if (procToClose) {
        if (WaitForSingleObject(procToClose, 0) == WAIT_TIMEOUT) {
            TerminateProcess(procToClose, 0);
            WaitForSingleObject(procToClose, 2000);
        }
        CloseHandle(procToClose);
    }
    if (jobToClose) {
        CloseHandle(jobToClose);
    }
    CloseHiddenTftpDesktop(deskToClose);
    if (!stagingDir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(stagingDir_), ec);
        stagingDir_.clear();
    }
}

bool TftpServer::IsListening() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsProcessAliveLocked();
}

std::string TftpServer::State() const {
    return IsListening() ? "listening" : "stopped";
}

std::string TftpServer::BindIp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bindIp_;
}

uint16_t TftpServer::Port() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return port_;
}

bool TftpServer::SetLocalRoot(const std::wstring& localDirectory, std::wstring& error) {
    std::wstring normalized;
    if (!ValidateLocalTftpDirectory(localDirectory, normalized, error)) {
        return false;
    }

    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    bool unchanged = false;
    std::string ip;
    uint16_t port = 69;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        unchanged = (rootConfig_.mode == TftpRootMode::Local && rootConfig_.localDirectory == normalized);
        rootConfig_.mode = TftpRootMode::Local;
        rootConfig_.localDirectory = std::move(normalized);
        ip = bindIp_;
        port = port_;
    }

    if (!unchanged && IsProcessAliveLocked()) {
        StopLocked();
        return StartLocked(ip, port, error);
    }
    return true;
}

void TftpServer::ClearRoot() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    bool wasNone = false;
    std::string ip;
    uint16_t port = 69;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wasNone = (rootConfig_.mode == TftpRootMode::None && rootConfig_.localDirectory.empty());
        rootConfig_ = {};
        ip = bindIp_;
        port = port_;
    }
    if (!wasNone && IsProcessAliveLocked()) {
        StopLocked();
        std::wstring ignoredError;
        StartLocked(ip, port, ignoredError);
    }
}

TftpRootConfig TftpServer::RootConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rootConfig_;
}

bool TftpServer::PublishLocal(const std::string& tftpName, const std::wstring& localPath,
                              bool allowOverwrite, std::wstring& error) {
    if (tftpName.empty()) {
        error = L"发布名不能为空";
        return false;
    }
    if (tftpName.find("..") != std::string::npos || tftpName.find('/') != std::string::npos ||
        tftpName.find('\\') != std::string::npos || tftpName.find(':') != std::string::npos) {
        error = L"发布名不能包含路径分隔符或 '..'";
        return false;
    }

    auto source = std::make_shared<LocalTftpFileSource>(localPath);
    if (source->Info().status != "Ready") {
        error = source->Info().error.empty() ? L"本地文件无法读取" : source->Info().error;
        return false;
    }

    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    bool isRunning = false;
    bool usingPublishedRoot = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!allowOverwrite && publishedFiles_.find(tftpName) != publishedFiles_.end()) {
            error = L"TFTP 发布名已存在: " + std::wstring(tftpName.begin(), tftpName.end());
            return false;
        }
        publishedFiles_[tftpName] = source;
        isRunning = IsProcessAliveLocked();
        usingPublishedRoot = (rootConfig_.mode == TftpRootMode::None);
    }
    if (isRunning && usingPublishedRoot) {
        return SyncPublishedFilesLocked(error);
    }
    return true;
}

bool TftpServer::Unpublish(const std::string& tftpName) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    bool erased = false;
    bool isRunning = false;
    bool usingPublishedRoot = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        erased = publishedFiles_.erase(tftpName) > 0;
        isRunning = IsProcessAliveLocked();
        usingPublishedRoot = (rootConfig_.mode == TftpRootMode::None);
    }
    if (erased && isRunning && usingPublishedRoot) {
        std::wstring ignored;
        SyncPublishedFilesLocked(ignored);
    }
    return erased;
}

std::vector<TftpPublishedItem> TftpServer::ListPublished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TftpPublishedItem> list;
    list.reserve(publishedFiles_.size());
    for (const auto& pair : publishedFiles_) {
        list.push_back({pair.first, pair.second});
    }
    return list;
}

size_t TftpServer::PublishedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publishedFiles_.size();
}

std::vector<TftpTransferStatus> TftpServer::ListTransfers() const {
    return {};
}

bool TftpServer::CancelTransfer(uint64_t /*transferId*/) {
    return false;
}

size_t TftpServer::ActiveTransferCount() const {
    return 0;
}

} // namespace serialctl
