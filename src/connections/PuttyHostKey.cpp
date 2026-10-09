#include "PuttyHostKey.h"

#include "ChildProcess.h"
#include "Win32Helpers.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <vector>

namespace serialctl {
namespace {

std::mutex cacheMutex;
std::map<std::wstring, std::wstring> hostKeyCache;

const wchar_t* TrustRegistryPath = L"Software\\SerialCtl\\TrustedHostKeys";

std::wstring ReadTrustedKey(const std::wstring& name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TrustRegistryPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};
    DWORD type = 0, size = 0;
    std::wstring value;
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_SZ && size >= sizeof(wchar_t) && size <= 4096) {
        std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, name.c_str(), nullptr, &type,
                reinterpret_cast<BYTE*>(buffer.data()), &size) == ERROR_SUCCESS && type == REG_SZ)
            value = buffer.data();
    }
    RegCloseKey(key);
    return value;
}

bool SaveTrustedKey(const std::wstring& name, const std::wstring& value, std::wstring& error) {
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, TrustRegistryPath, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(key, name.c_str(), 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS) error = L"无法保存已确认的 SSH 主机密钥：" + Win32ErrorMessage(result);
    return result == ERROR_SUCCESS;
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring Decode(const std::string& bytes) {
    if (bytes.empty()) return {};
    const int utf8 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    return MultiByteToWide(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
        utf8 > 0 ? CP_UTF8 : CP_ACP);
}

bool RunProbe(const std::wstring& executable, const std::wstring& host, std::uint16_t port,
    const std::wstring& username, const std::wstring& password, const std::wstring& hostKey,
    DWORD& exitCode, std::wstring& output, std::wstring& error,
    const std::atomic_bool* cancel) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &security, 0)) {
        error = L"创建 SSH 验证管道失败：" + Win32ErrorMessage();
        return false;
    }
    if (!SetHandleInheritable(outputRead, false,
            L"设置 SSH 验证管道读取端继承属性失败", error)) {
        CloseHandle(outputRead);
        CloseHandle(outputWrite);
        return false;
    }
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        error = L"打开 SSH 验证空输入失败：" + Win32ErrorMessage();
        CloseHandle(outputRead);
        CloseHandle(outputWrite);
        return false;
    }

    std::wstring command = QuoteCommandLineArgument(executable) + L" -batch -ssh -P " +
        std::to_wstring(port) + L" -l " + QuoteCommandLineArgument(username);
    if (!password.empty()) command += L" -pw " + QuoteCommandLineArgument(password);
    if (!hostKey.empty()) command += L" -hostkey " + QuoteCommandLineArgument(hostKey);
    command += L" " + QuoteCommandLineArgument(host) + L" exit";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    RestrictedProcessStartup startup;
    if (!startup.Initialize(nullInput, outputWrite, outputWrite, {}, error)) {
        CloseHandle(nullInput);
        CloseHandle(outputWrite);
        CloseHandle(outputRead);
        return false;
    }
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable.c_str(), buffer.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        startup.StartupInfo(), &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(nullInput);
    CloseHandle(outputWrite);
    if (!created) {
        CloseHandle(outputRead);
        error = L"启动 SSH 验证失败：" + Win32ErrorMessage(createError);
        return false;
    }

    std::string bytes;
    std::array<char, 4096> chunk{};
    const DWORD started = GetTickCount();
    bool timedOut = false;
    bool cancelled = false;
    for (;;) {
        DWORD available = 0;
        while (PeekNamedPipe(outputRead, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD read = 0;
            if (!ReadFile(outputRead, chunk.data(),
                    std::min<DWORD>(available, static_cast<DWORD>(chunk.size())), &read, nullptr) || !read) break;
            bytes.append(chunk.data(), read);
        }
        if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) break;
        if (cancel && cancel->load()) {
            cancelled = true;
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
        if (GetTickCount() - started > 15000) {
            timedOut = true;
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
    }
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(outputRead, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr) || !read) break;
        bytes.append(chunk.data(), read);
    }
    exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(outputRead);
    output = Decode(bytes);
    if (cancelled) {
        error = L"SSH 登录验证已取消。";
        return false;
    }
    if (timedOut) {
        error = L"SSH 登录验证超时。";
        return false;
    }
    return true;
}

std::wstring ExtractFingerprint(const std::wstring& output) {
    const std::wstring marker = L"key fingerprint is:";
    size_t position = output.find(marker);
    if (position == std::wstring::npos) return {};
    position = output.find_first_of(L"\r\n", position + marker.size());
    if (position == std::wstring::npos) return {};
    position = output.find_first_not_of(L"\r\n \t", position);
    if (position == std::wstring::npos) return {};
    const size_t end = output.find_first_of(L"\r\n", position);
    return Trim(output.substr(position, end == std::wstring::npos ? std::wstring::npos : end - position));
}

} // namespace

bool ResolvePuttyHostKey(const std::wstring& executable, const std::wstring& host,
    std::uint16_t port, const std::wstring& username, const std::wstring& password,
    std::wstring& hostKey, std::wstring& error, const std::atomic_bool* cancel,
    const HostKeyConfirmation& confirm) {
    const std::wstring cacheKey = host + L":" + std::to_wstring(port);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto found = hostKeyCache.find(cacheKey);
        if (found != hostKeyCache.end()) {
            hostKey = found->second;
            // The explicit fingerprint is still validated by every subsequent
            // plink/psftp process. Avoid an additional login probe for each
            // queued SFTP operation once this process has verified the key.
            return true;
        }
    }

    hostKey = ReadTrustedKey(cacheKey);
    DWORD exitCode = 1;
    std::wstring output;
    if (!RunProbe(executable, host, port, username, password, hostKey,
            exitCode, output, error, cancel)) return false;
    if (exitCode == 0) {
        // PuTTY already knows and has validated this host through its persisted
        // host-key cache. Remember that state in-process too; callers must omit
        // -hostkey when no explicit fingerprint was needed.
        std::lock_guard<std::mutex> lock(cacheMutex);
        hostKeyCache[cacheKey] = hostKey;
        return true;
    }

    // Only bootstrap an explicit fingerprint for a genuinely unknown host.
    // A changed cached key also prints a new fingerprint, but accepting that
    // automatically would defeat PuTTY's man-in-the-middle protection.
    const bool unknownHost = output.find(
        L"The host key is not cached for this server") != std::wstring::npos;
    const std::wstring discovered = unknownHost ? ExtractFingerprint(output) : std::wstring();
    if (!discovered.empty() && hostKey.empty()) {
        if (!confirm || !confirm(host, port, discovered) || (cancel && cancel->load())) {
            error = L"未信任该 SSH 主机。请通过独立渠道核对主机指纹后再连接。\n" + discovered;
            return false;
        }
        if (!SaveTrustedKey(cacheKey, discovered, error)) return false;
        hostKey = discovered;
        output.clear();
        if (!RunProbe(executable, host, port, username, password, hostKey,
                exitCode, output, error, cancel)) return false;
        if (exitCode == 0) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            hostKeyCache[cacheKey] = hostKey;
            return true;
        }
    }

    error = Trim(output);
    if (error.empty()) error = L"SSH 登录验证失败，退出码 " + std::to_wstring(exitCode);
    if (error.size() > 600) error = error.substr(error.size() - 600);
    return false;
}

} // namespace serialctl
