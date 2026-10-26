#include "PuttyHostKey.h"

#include "ChildProcess.h"
#include "PasswordPipe.h"
#include "ProcessOutput.h"
#include "Win32Helpers.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <map>
#include <mutex>
#include <vector>

namespace serialctl {
namespace {

std::mutex cacheMutex;
std::map<std::wstring, std::wstring> hostKeyCache;
std::map<std::wstring, unsigned long long> hostKeyRevisions;

const wchar_t* TrustRegistryPath = L"Software\\SerialCtl\\TrustedHostKeys";

std::wstring EndpointName(const std::wstring& host, std::uint16_t port) {
    auto name = host + L":" + std::to_wstring(port);
    // Registry value names are case insensitive. The shared in-memory pin and
    // revision must use the same rule across SSH, AI and SFTP.
    std::transform(name.begin(), name.end(), name.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return name;
}

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
    PasswordPipe passwordPipe;
    if (!password.empty() && !passwordPipe.Open(password, error)) {
        if (error.empty()) error = L"无法创建安全密码通道";
        return false;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &security, 65536)) {
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

    // PuTTY logs the actual fingerprint before checking an explicit -hostkey.
    // In batch mode a mismatch still aborts before password authentication.
    std::wstring command = QuoteCommandLineArgument(executable) + L" -batch -v -ssh -P " +
        std::to_wstring(port) + L" -l " + QuoteCommandLineArgument(username);
    if (!password.empty()) command += L" -pwfile " + QuoteCommandLineArgument(passwordPipe.Name());
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
    const DWORD started = GetTickCount();
    bool timedOut = false;
    bool cancelled = false;
    bool tooLarge = false;
    for (;;) {
        passwordPipe.Poll();
        const auto read = ReadProcessOutput(outputRead, bytes, 256 * 1024, cancel);
        tooLarge = read == PipeRead::Limit;
        cancelled = read == PipeRead::Cancelled;
        if (tooLarge || cancelled || passwordPipe.Failed()) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
        if (WaitForSingleObject(process.hProcess, read == PipeRead::More ? 0 : 20) == WAIT_OBJECT_0) break;
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
    if (!tooLarge && !cancelled && !timedOut) {
        PipeRead read;
        do { read = ReadProcessOutput(outputRead, bytes, 256 * 1024, cancel); }
        while (read == PipeRead::More);
        tooLarge = read == PipeRead::Limit;
        cancelled = read == PipeRead::Cancelled;
    }
    exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(outputRead);
    output = Decode(bytes);
    if (tooLarge || passwordPipe.Failed()) {
        error = tooLarge ? L"SSH 验证输出超过 256 KiB，已停止连接" : L"安全密码通道写入失败";
        return false;
    }
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

bool ValidFingerprint(const std::wstring& value) {
    const auto position = value.find(L"SHA256:");
    if (position == std::wstring::npos || position > 160) return false;
    const auto digest = value.substr(position + 7);
    if (digest.size() != 43 && !(digest.size() == 44 && digest.back() == L'=')) return false;
    for (size_t i = 0; i < 43; ++i) {
        const wchar_t c = digest[i];
        if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
              (c >= L'0' && c <= L'9') || c == L'+' || c == L'/')) return false;
    }
    const auto prefix = value.substr(0, position);
    if (!prefix.empty() && prefix.find(L"ssh-") != 0 && prefix.find(L"ecdsa-") != 0) return false;
    return std::all_of(prefix.begin(), prefix.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'-' || c == L'@' || c == L'.' || c == L' ';
    });
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
    const auto value = Trim(output.substr(position, end == std::wstring::npos ? std::wstring::npos : end - position));
    return ValidFingerprint(value) ? value : std::wstring();
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

bool SameFingerprint(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    const auto first = a.find(L"SHA256:"), second = b.find(L"SHA256:");
    return first != std::wstring::npos && second != std::wstring::npos
        ? a.substr(first) == b.substr(second) : a == b;
}

} // namespace

std::wstring TrustedPuttyHostKey(const std::wstring& host, std::uint16_t port) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    return ReadTrustedKey(EndpointName(host, port));
}

bool IsPuttyHostKeyFailure(const std::wstring& message) {
    const auto lower = Lower(message);
    return lower.find(L"host key not in manually configured list") != std::wstring::npos ||
        lower.find(L"host key did not match manually configured list") != std::wstring::npos ||
        lower.find(L"host key does not match") != std::wstring::npos ||
        lower.find(L"host key is not cached") != std::wstring::npos ||
        lower.find(L"potential security breach") != std::wstring::npos;
}

bool IsPuttyAuthenticationFailure(const std::wstring& message) {
    const auto lower = Lower(message);
    return lower.find(L"access denied") != std::wstring::npos ||
        lower.find(L"authentication failed") != std::wstring::npos ||
        lower.find(L"no supported authentication methods") != std::wstring::npos ||
        lower.find(L"认证失败") != std::wstring::npos;
}

bool ResolvePuttyHostKey(const std::wstring& executable, const std::wstring& host,
    std::uint16_t port, const std::wstring& username, const std::wstring& password,
    std::wstring& hostKey, std::wstring& error, const std::atomic_bool* cancel,
    const HostKeyConfirmation& confirm, bool reverify) {
    const std::wstring cacheKey = EndpointName(host, port);
    std::wstring previous;
    unsigned long long revision = 0;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto found = hostKeyCache.find(cacheKey);
        if (found != hostKeyCache.end() && !confirm && !reverify) {
            hostKey = found->second;
            // The explicit fingerprint is still validated by every subsequent
            // plink/psftp process. Avoid an additional login probe for each
            // queued SFTP operation once this process has verified the key.
            return true;
        }
        previous = ReadTrustedKey(cacheKey);
        revision = hostKeyRevisions[cacheKey];
    }

    if (!previous.empty() && !ValidFingerprint(previous) && !reverify) {
        error = L"已保存的 SSH 主机指纹格式无效，请在连接窗口选择“重新核验主机”。";
        return false;
    }
    hostKey = ValidFingerprint(previous) ? previous : std::wstring();
    DWORD exitCode = 1;
    std::wstring output;
    if (!RunProbe(executable, host, port, username, password, hostKey,
            exitCode, output, error, cancel)) return false;
    auto remember = [&](const std::wstring& fingerprint, bool persist, bool cache) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (hostKeyRevisions[cacheKey] != revision || ReadTrustedKey(cacheKey) != previous) {
            error = L"此服务器的主机身份已由另一连接更新，请重新连接核验。";
            return false;
        }
        if (cancel && cancel->load()) { error = L"SSH 主机身份核验已取消。"; return false; }
        if (persist) {
            if (!SaveTrustedKey(cacheKey, fingerprint, error)) return false;
            ++hostKeyRevisions[cacheKey];
            hostKeyCache.erase(cacheKey);
        }
        if (cache) hostKeyCache[cacheKey] = fingerprint;
        hostKey = fingerprint;
        return true;
    };

    auto discovered = ExtractFingerprint(output);
    if (exitCode != 0 && IsPuttyHostKeyFailure(output) && discovered.empty()) {
        error = L"未能取得完整的 SSH 主机指纹，未更新身份记录。请重新核验主机。";
        return false;
    }
    if (exitCode == 0 && !reverify) {
        // Migrate an already validated PuTTY identity to an explicit SerialCtl
        // pin when verbose output supplies it. No PuTTY registry key is changed.
        const auto fingerprint = previous.empty() ? discovered : previous;
        return remember(fingerprint, previous.empty() && !fingerprint.empty(), true);
    }
    if (exitCode == 0 && discovered.empty()) discovered = previous;
    const bool unknownHost = previous.empty() && Lower(output).find(L"host key is not cached") != std::wstring::npos;
    const bool changedHost = IsPuttyHostKeyFailure(output) && !unknownHost;
    if (!discovered.empty() && (exitCode == 0 || unknownHost || changedHost)) {
        HostKeyChallenge challenge{host, port, previous, discovered,
            changedHost ? HostKeyReason::Changed : reverify && !unknownHost ? HostKeyReason::Reverify : HostKeyReason::FirstConnection};
        if (!confirm || !confirm(challenge) || (cancel && cancel->load())) {
            error = changedHost ? L"SSH 主机身份已变化，未更新原记录。请重新连接并核对新指纹。"
                                : L"未信任该 SSH 主机。请通过独立渠道核对主机指纹后再连接。";
            return false;
        }
        if (exitCode == 0 && SameFingerprint(discovered, previous))
            return remember(discovered, true, true);
        // Validate the exact approved identity again. A second key change,
        // timeout or cancellation cannot erase/overwrite the previous pin.
        output.clear();
        if (!RunProbe(executable, host, port, username, password, discovered,
                exitCode, output, error, cancel)) return false;
        if (exitCode != 0 && IsPuttyHostKeyFailure(output)) {
            error = L"服务器的主机身份在核验期间再次变化，未更新原记录。请重新核验。";
            return false;
        }
        const auto verified = ExtractFingerprint(output);
        if (exitCode == 0 || (IsPuttyAuthenticationFailure(output) && !IsPuttyHostKeyFailure(output) && SameFingerprint(verified, discovered))) {
            // Authentication may fail after identity verification. Keep the
            // user-approved identity and handle the password separately.
            if (!remember(discovered, true, exitCode == 0)) return false;
            if (exitCode == 0) return true;
        }
    }

    if (IsPuttyAuthenticationFailure(output)) {
        error = L"SSH 认证失败，请检查用户名、密码或服务器认证方式。";
        return false;
    }
    const auto fatal = output.rfind(L"FATAL ERROR:");
    error = Trim(fatal == std::wstring::npos ? output : output.substr(fatal));
    if (error.empty()) error = L"SSH 登录验证失败，退出码 " + std::to_wstring(exitCode);
    if (error.size() > 600) error = error.substr(error.size() - 600);
    return false;
}

} // namespace serialctl
