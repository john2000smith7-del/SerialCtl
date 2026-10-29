#include "NetworkConnect.h"
#include "ChildProcess.h"
#include "FilePaths.h"
#include "ProcessOutput.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <cstring>

namespace serialctl {
namespace {
constexpr size_t MaximumAddresses = 64;
struct ResolverHeader { int status = 0; unsigned count = 0; };
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
struct Child {
    PROCESS_INFORMATION process{};
    ~Child() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 1000);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
    }
};
bool Cancelled(const std::atomic_bool *cancel) { return cancel && cancel->load(); }
bool Expired(ULONGLONG deadline) { return GetTickCount64() >= deadline; }
void CopyAddresses(ADDRINFOW *list, TcpAddresses &addresses) {
    for (auto *item = list; item && addresses.size() < MaximumAddresses; item = item->ai_next) {
        if ((item->ai_family != AF_INET && item->ai_family != AF_INET6) || !item->ai_addr ||
            item->ai_addrlen > sizeof(sockaddr_storage) ||
            item->ai_addrlen < (item->ai_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6))) continue;
        TcpAddress address{};
        address.length = static_cast<int>(item->ai_addrlen);
        std::memcpy(&address.address, item->ai_addr, item->ai_addrlen);
        addresses.push_back(address);
    }
}
int ResolveDirect(const std::wstring &host, bool numericOnly, TcpAddresses &addresses) {
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = numericOnly ? AI_NUMERICHOST : 0;
    ADDRINFOW *list = nullptr;
    const int status = GetAddrInfoW(host.c_str(), L"0", &hints, &list);
    if (!status) { CopyAddresses(list, addresses); FreeAddrInfoW(list); }
    return status;
}
bool WriteAll(HANDLE handle, const void *data, DWORD length) {
    auto bytes = static_cast<const char *>(data);
    while (length) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes, length, &written, nullptr) || !written) return false;
        bytes += written; length -= written;
    }
    return true;
}
} // namespace

int RunNetworkResolver(const std::wstring &host) {
    WSADATA data{};
    ResolverHeader header{};
    TcpAddresses addresses;
    const int startup = WSAStartup(MAKEWORD(2, 2), &data);
    header.status = startup ? startup : ResolveDirect(host, false, addresses);
    header.count = static_cast<unsigned>(addresses.size());
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool written = output && output != INVALID_HANDLE_VALUE && WriteAll(output, &header, sizeof(header)) &&
        (addresses.empty() || WriteAll(output, addresses.data(), static_cast<DWORD>(addresses.size() * sizeof(TcpAddress))));
    if (!startup) WSACleanup();
    return written ? 0 : 1;
}

bool ResolveTcpAddresses(const std::wstring &host, ULONGLONG deadline, TcpAddresses &addresses,
                         std::wstring &error, const std::atomic_bool *cancel) {
    addresses.clear();
    if (Cancelled(cancel)) { error = L"连接已取消。"; return false; }
    if (Expired(deadline)) { error = L"主机解析超时。"; return false; }
    if (host.empty() || host.size() > 32760) { error = L"主机名无效。"; return false; }
    // AI_NUMERICHOST forbids DNS. Local and remote serial IP addresses take
    // this direct path without starting a helper process.
    const int numeric = ResolveDirect(host, true, addresses);
    if (!numeric && !addresses.empty()) {
        if (Cancelled(cancel) || Expired(deadline)) {
            error = Cancelled(cancel) ? L"连接已取消。" : L"主机解析超时。"; return false;
        }
        return true;
    }
    addresses.clear();
    const auto executable = ExtendedFilePath(ModuleFilePath());
    if (executable.empty()) { error = L"无法定位主机解析程序。"; return false; }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle read, write, nullFile;
    if (!CreatePipe(&read.value, &write.value, &security, 65536) ||
        !SetHandleInheritable(read.value, false, L"设置解析管道", error)) {
        if (error.empty()) error = L"无法创建主机解析管道：" + Win32ErrorMessage();
        return false;
    }
    nullFile.value = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullFile.value == INVALID_HANDLE_VALUE) { error = L"无法打开解析程序输入。"; return false; }
    RestrictedProcessStartup startup;
    if (!startup.Initialize(nullFile.value, write.value, nullFile.value, {}, error)) return false;
    std::wstring command = QuoteCommandLineArgument(executable) + L" " + NetworkResolverArgument + L" " +
                           QuoteCommandLineArgument(host);
    std::vector<wchar_t> buffer(command.begin(), command.end()); buffer.push_back(L'\0');
    if (Cancelled(cancel) || Expired(deadline)) {
        error = Cancelled(cancel) ? L"连接已取消。" : L"主机解析超时。"; return false;
    }
    Child child;
    if (!CreateProcessW(executable.c_str(), buffer.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, startup.StartupInfo(), &child.process)) {
        error = L"无法启动主机解析程序：" + Win32ErrorMessage(); return false;
    }
    CloseHandle(write.value); write.value = nullptr;
    std::string bytes;
    const size_t maximum = sizeof(ResolverHeader) + MaximumAddresses * sizeof(TcpAddress);
    for (;;) {
        const auto readStatus = ReadProcessOutput(read.value, bytes, maximum, cancel);
        if (readStatus == PipeRead::Limit) { error = L"主机解析结果超出上限。"; return false; }
        if (Cancelled(cancel)) { error = L"连接已取消。"; return false; }
        const auto now = GetTickCount64();
        if (now >= deadline) { error = L"主机解析超时。"; return false; }
        const auto wait = static_cast<DWORD>((std::min)(ULONGLONG(25), deadline - now));
        const DWORD signal = WaitForSingleObject(child.process.hProcess, readStatus == PipeRead::More ? 0 : wait);
        if (signal == WAIT_OBJECT_0) break;
        if (signal == WAIT_FAILED) { error = L"等待主机解析失败。"; return false; }
    }
    PipeRead status;
    do { status = ReadProcessOutput(read.value, bytes, maximum, cancel); } while (status == PipeRead::More);
    if (Cancelled(cancel)) { error = L"连接已取消。"; return false; }
    if (Expired(deadline)) { error = L"主机解析超时。"; return false; }
    DWORD exitCode = 1;
    GetExitCodeProcess(child.process.hProcess, &exitCode);
    ResolverHeader header{};
    if (exitCode || status == PipeRead::Limit || bytes.size() < sizeof(header)) {
        error = L"主机解析程序未返回完整结果。"; return false;
    }
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.status || header.count > MaximumAddresses ||
        bytes.size() != sizeof(header) + header.count * sizeof(TcpAddress)) {
        error = L"无法解析主机名，错误码：" + std::to_wstring(header.status); return false;
    }
    addresses.resize(header.count);
    if (header.count) std::memcpy(addresses.data(), bytes.data() + sizeof(header), header.count * sizeof(TcpAddress));
    for (const auto &address : addresses) {
        if ((address.address.ss_family != AF_INET && address.address.ss_family != AF_INET6) ||
            address.length > sizeof(sockaddr_storage) || address.length <
                static_cast<int>(address.address.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6))) {
            addresses.clear(); error = L"主机解析结果格式无效。"; return false;
        }
    }
    if (addresses.empty()) { error = L"主机名没有可用地址。"; return false; }
    return true;
}

SOCKET ConnectTcpSocket(const TcpAddresses &addresses, std::uint16_t port, ULONGLONG deadline,
                        std::wstring &error, const std::atomic_bool *cancel) {
    int lastError = WSAETIMEDOUT;
    for (size_t index = 0; index < addresses.size() && !Cancelled(cancel) && !Expired(deadline); ++index) {
        auto address = addresses[index];
        if (address.address.ss_family == AF_INET)
            reinterpret_cast<sockaddr_in *>(&address.address)->sin_port = htons(port);
        else
            reinterpret_cast<sockaddr_in6 *>(&address.address)->sin6_port = htons(port);
        const auto now = GetTickCount64();
        const auto remaining = deadline > now ? deadline - now : 0;
        // Reserve time for every remaining IPv4/IPv6 candidate, even when the
        // caller's total budget is shorter than a single default connect wait.
        const auto slice = (std::min)(remaining, (std::max)(ULONGLONG(1), remaining / (addresses.size() - index)));
        const auto candidateDeadline = now + slice;
        SOCKET candidate = socket(address.address.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (candidate == INVALID_SOCKET) { lastError = WSAGetLastError(); continue; }
        u_long nonBlocking = 1;
        if (ioctlsocket(candidate, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            lastError = WSAGetLastError(); closesocket(candidate); continue;
        }
        bool ready = connect(candidate, reinterpret_cast<sockaddr *>(&address.address), address.length) == 0;
        lastError = ready ? 0 : WSAGetLastError();
        if (!ready && lastError == WSAEWOULDBLOCK) {
            while (!Cancelled(cancel)) {
                const auto current = GetTickCount64();
                if (current >= candidateDeadline || current >= deadline) break;
                const auto remainingWait = (std::min)(ULONGLONG(25), candidateDeadline - current);
                timeval interval{0, static_cast<long>(remainingWait * 1000)};
                fd_set writable{}, failed{};
                FD_ZERO(&writable); FD_ZERO(&failed); FD_SET(candidate, &writable); FD_SET(candidate, &failed);
                const int selected = select(0, nullptr, &writable, &failed, &interval);
                if (!selected) continue;
                int size = sizeof(lastError);
                ready = selected > 0 && getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                    reinterpret_cast<char *>(&lastError), &size) == 0 && lastError == 0;
                break;
            }
        }
        nonBlocking = 0;
        const bool blocking = ioctlsocket(candidate, FIONBIO, &nonBlocking) == 0;
        if (ready && blocking && !Cancelled(cancel) && !Expired(deadline)) return candidate;
        closesocket(candidate);
    }
    error = Cancelled(cancel) ? L"连接已取消。" :
        L"连接失败或超时：" + SocketErrorMessage(lastError == WSAEWOULDBLOCK ? WSAETIMEDOUT : lastError);
    return INVALID_SOCKET;
}

SOCKET ConnectTcpSocket(const std::wstring &host, std::uint16_t port, std::wstring &error,
                        const std::atomic_bool *cancel, DWORD timeoutMs) {
    const auto deadline = GetTickCount64() + timeoutMs;
    TcpAddresses addresses;
    if (!ResolveTcpAddresses(host, deadline, addresses, error, cancel)) return INVALID_SOCKET;
    return ConnectTcpSocket(addresses, port, deadline, error, cancel);
}
} // namespace serialctl
