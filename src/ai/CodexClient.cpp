#include "CodexClient.h"
#include "ChildProcess.h"
#include "PasswordPipe.h"
#include "Win32Helpers.h"
#include <array>
#include <cwctype>
#include <wincred.h>
#include <windows.h>

namespace serialctl::ai {
namespace {
const wchar_t *SettingsKey = L"Software\\SerialCtl\\AI";
std::wstring CredentialName(const ServerSettings &s) {
    return L"SerialCtl/AI/" + s.host + L":" + std::to_wstring(s.port) + L"/" + s.username;
}
std::wstring ReadString(HKEY key, const wchar_t *name) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size > 32768)
        return {};
    std::vector<wchar_t> text(size / sizeof(wchar_t) + 1);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(text.data()), &size) != ERROR_SUCCESS)
        return {};
    return text.data();
}
void Close(HANDLE &h) {
    if (h && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    h = nullptr;
}
} // namespace
bool IsAuthenticationFailure(const std::wstring &message) {
    return IsPuttyAuthenticationFailure(message);
}
bool ForgetSavedServerPassword(const ServerSettings &s, std::wstring &error) {
    if (CredDeleteW(CredentialName(s).c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND)
        return true;
    error = L"无法删除旧的 Windows 密码凭据：" + Win32ErrorMessage();
    return false;
}
bool ValidateServer(const ServerSettings &s, std::wstring &error) {
    if (s.host.empty() || s.host.size() > 253 || s.host.front() == L'-' ||
        s.host.find_first_of(L" \t\r\n/\\\"'@") != std::wstring::npos)
        error = L"请输入有效的服务器 IP 地址或域名";
    else if (!s.port || s.port > 65535)
        error = L"SSH 端口必须为 1–65535";
    else if (s.username.empty() || s.username.size() > 128 || s.username.front() == L'-' ||
             s.username.find_first_of(L"\r\n\t ") != std::wstring::npos)
        error = L"请输入有效的 SSH 用户名";
    else if (s.password.empty() || s.password.size() > 1024 || s.password.find_first_of(L"\r\n") != std::wstring::npos)
        error = L"请输入 SSH 密码（不支持换行）";
    else if (s.workingDirectory.size() > 4096 || (!s.workingDirectory.empty() && s.workingDirectory.front() != L'/') ||
             s.workingDirectory.find_first_of(L"\r\n") != std::wstring::npos || s.workingDirectory.find(L'\0') != std::wstring::npos)
        error = L"项目目录请填写 Linux 绝对路径，或留空使用服务器默认目录";
    else
        return true;
    return false;
}
ServerSettings LoadServerSettings() {
    ServerSettings s;
    s.username = L"linux";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SettingsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return s;
    s.host = ReadString(key, L"Host");
    s.username = ReadString(key, L"Username");
    if (s.username.empty()) s.username = L"linux";
    s.workingDirectory = ReadString(key, L"WorkingDirectory");
    DWORD port = 22, size = sizeof(port), type = REG_DWORD;
    RegQueryValueExW(key, L"Port", nullptr, &type, reinterpret_cast<BYTE *>(&port), &size);
    s.port = port > 0 && port <= 65535 ? port : 22;
    RegCloseKey(key);
    PCREDENTIALW credential = nullptr;
    if (CredReadW(CredentialName(s).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        if (credential->CredentialBlobSize <= 2048 && credential->CredentialBlobSize % sizeof(wchar_t) == 0) {
            s.password.assign(reinterpret_cast<wchar_t *>(credential->CredentialBlob),
                              credential->CredentialBlobSize / sizeof(wchar_t));
            s.rememberPassword = true;
        }
        CredFree(credential);
    }
    return s;
}
bool SaveServerSettings(const ServerSettings &s, std::wstring &error) {
    const auto previous = LoadServerSettings();
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, SettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result == ERROR_SUCCESS) {
        auto set = [&](const wchar_t *name, const std::wstring &text) {
            if (result == ERROR_SUCCESS)
                result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(text.c_str()),
                                        static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
        };
        set(L"Host", s.host);
        set(L"Username", s.username);
        set(L"WorkingDirectory", s.workingDirectory);
        DWORD port = s.port;
        if (result == ERROR_SUCCESS)
            result = RegSetValueExW(key, L"Port", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&port), sizeof(port));
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS) {
        error = L"保存 AI 设置失败：" + Win32ErrorMessage(result);
        return false;
    }
    if (!s.rememberPassword || CredentialName(previous) != CredentialName(s))
        CredDeleteW(CredentialName(previous).c_str(), CRED_TYPE_GENERIC, 0);
    if (s.rememberPassword) {
        CREDENTIALW c{};
        c.Type = CRED_TYPE_GENERIC;
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        auto name = CredentialName(s);
        c.TargetName = name.data();
        c.UserName = const_cast<wchar_t *>(s.username.c_str());
        c.CredentialBlob = reinterpret_cast<BYTE *>(const_cast<wchar_t *>(s.password.data()));
        c.CredentialBlobSize = static_cast<DWORD>(s.password.size() * sizeof(wchar_t));
        if (!CredWriteW(&c, 0)) {
            error = L"Windows 凭据存储失败；未将密码写入设置：" + Win32ErrorMessage();
            return false;
        }
    }
    return true;
}
CodexClient::~CodexClient() {
    Shutdown();
}
bool CodexClient::Connect(ServerSettings settings, Event event, HostKeyConfirmation confirm, bool reverifyHostKey) {
    if (running_)
        return false;
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        writes_.clear();
        queuedBytes_ = 0;
        event_ = std::move(event);
    }
    stop_ = false;
    ready_ = false;
    running_ = true;
    worker_ = std::thread([this, settings = std::move(settings), confirm = std::move(confirm), reverifyHostKey]() mutable {
        try {
            Run(settings, confirm, reverifyHostKey);
        } catch (const std::exception &) {
            Emit({{"event", "error"}, {"message", u8"Codex 通信或协议异常；连接已关闭"}});
        }
        if (!settings.password.empty())
            SecureZeroMemory(settings.password.data(), settings.password.size() * sizeof(wchar_t));
        ready_ = false;
        running_ = false;
        Emit({{"event", "disconnected"}});
    });
    return true;
}
void CodexClient::Disconnect() {
    stop_ = true;
    ready_ = false;
    if (worker_.joinable())
        CancelSynchronousIo(reinterpret_cast<HANDLE>(worker_.native_handle()));
}
void CodexClient::Shutdown() {
    Disconnect();
    if (worker_.joinable())
        worker_.join();
}
void CodexClient::Emit(Json event) {
    if (event_)
        event_(std::move(event));
}
bool CodexClient::Send(Json message) {
    std::string bytes = message.dump() + "\n";
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || stop_ || bytes.size() + queuedBytes_ > 256 * 1024)
        return false;
    queuedBytes_ += bytes.size();
    writes_.push_back(std::move(bytes));
    return true;
}
void CodexClient::Run(ServerSettings settings, HostKeyConfirmation confirm, bool reverifyHostKey) {
    std::wstring error;
    if (!ValidateServer(settings, error)) {
        Emit({{"event", "error"}, {"message", WideToMultiByte(error, CP_UTF8)}});
        return;
    }
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    std::wstring plink(module);
    plink.resize(plink.find_last_of(L"\\/") + 1);
    plink += L"plink.exe";
    if (GetFileAttributesW(plink.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Emit({{"event", "error"}, {"message", u8"未找到配套 plink.exe"}});
        return;
    }
    Emit({{"event", "status"}, {"message", u8"正在验证 SSH 连接与主机身份…"}});
    std::wstring fingerprint;
    if (!ResolvePuttyHostKey(plink, settings.host, static_cast<uint16_t>(settings.port), settings.username,
                             settings.password, fingerprint, error, &stop_, confirm, reverifyHostKey)) {
        // SSH output is never retained as a transcript or diagnostic log.
        const bool auth = IsAuthenticationFailure(error);
        Emit({{"event", "error"},
              {"category", auth ? "authentication" : "connection"},
              {"message", auth ? std::string(u8"SSH 认证失败，请输入新的密码") : WideToMultiByte(error, CP_UTF8)}});
        return;
    }
    if (stop_)
        return;
    Emit({{"event", "status"}, {"message", u8"SSH 已核验；正在检测 Codex 并启动 App Server…"}});
    PasswordPipe password;
    if (!password.Open(settings.password, error)) {
        Emit({{"event", "error"}, {"message", u8"安全密码通道创建失败"}});
        return;
    }
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr,
           errWrite = nullptr;
    HANDLE process = nullptr, processThread = nullptr;
    struct Cleanup {
        HANDLE &a, &b, &c, &d, &e, &f, &process, &thread;
        ~Cleanup() {
            if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 1000);
            }
            Close(a);
            Close(b);
            Close(c);
            Close(d);
            Close(e);
            Close(f);
            Close(thread);
            Close(process);
        }
    } cleanup{inRead, inWrite, outRead, outWrite, errRead, errWrite, process, processThread};
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0) ||
        !CreatePipe(&errRead, &errWrite, &sa, 0) || !SetHandleInheritable(inWrite, false, L"AI 输入", error) ||
        !SetHandleInheritable(outRead, false, L"AI 输出", error) ||
        !SetHandleInheritable(errRead, false, L"AI 错误输出", error)) {
        Emit({{"event", "error"}, {"message", u8"创建 AI stdio 通道失败"}});
        return;
    }
    std::wstring command = QuoteCommandLineArgument(plink) +
                           L" -batch -ssh -T -no-antispoof -no-sanitise-stdout -no-sanitise-stderr -P " +
                           std::to_wstring(settings.port) + L" -l " + QuoteCommandLineArgument(settings.username) +
                           L" -pwfile " + QuoteCommandLineArgument(password.Name());
    if (!fingerprint.empty())
        command += L" -hostkey " + QuoteCommandLineArgument(fingerprint);
    // Constant command; no host/user/model text is interpolated into a shell.
    command += L" " + QuoteCommandLineArgument(settings.host) + L" " +
               QuoteCommandLineArgument(L"sh -lc 'if [ -r \"$HOME/.config/serialctl/codex-env.sh\" ]; then "
                                        L". \"$HOME/.config/serialctl/codex-env.sh\" 1>&2; fi; "
                                        L"command -v codex >/dev/null 2>&1 || { echo SERIALCTL_CODEX_NOT_FOUND "
                                        L">&2; exit 127; }; exec codex app-server --listen stdio://'");
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    RestrictedProcessStartup startup;
    if (!startup.Initialize(inRead, outWrite, errWrite, {}, error)) {
        Emit({{"event", "error"}, {"message", u8"限制 AI 子进程继承失败"}});
        return;
    }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(plink.c_str(), buffer.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, startup.StartupInfo(),
                        &pi)) {
        Emit({{"event", "error"}, {"message", u8"无法启动 SSH App Server 进程"}});
        return;
    }
    process = pi.hProcess;
    processThread = pi.hThread;
    Close(inRead);
    Close(outWrite);
    Close(errWrite);
    Send({{"id", 0},
          {"method", "initialize"},
          {"params",
           {{"clientInfo", {{"name", "serialctl_ai"}, {"title", "SerialCtl AI"}, {"version", "1.0"}}},
            {"capabilities", {{"experimentalApi", true}}}}}});
    JsonLines frames;
    std::string stderrText;
    const ULONGLONG started = GetTickCount64();
    ULONGLONG lastRead = started, heartbeatAt = 0;
    std::string sending;
    size_t sent = 0;
    while (!stop_) {
        password.Poll();
        auto read = [&](HANDLE pipe, bool standardOutput) {
            DWORD available = 0;
            std::array<char, 8192> data{};
            // Per-poll budget prevents an output flood starving cancellation/writes.
            for (size_t budget = 0;
                 budget < 65536 && PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available;
                 budget += data.size()) {
                DWORD n = 0;
                if (!ReadFile(pipe, data.data(), std::min<DWORD>(available, static_cast<DWORD>(data.size())), &n,
                              nullptr) ||
                    !n)
                    break;
                if (standardOutput) {
                    lastRead = GetTickCount64();
                    for (auto &frame : frames.Feed(data.data(), n)) {
                        if (frame.value("id", Json()) == "serialctl-heartbeat")
                            heartbeatAt = 0;
                        if (frame.contains("id") && frame["id"] == 0 && !frame.contains("method")) {
                            if (frame.contains("error"))
                                throw std::runtime_error("initialize rejected");
                            Send({{"method", "initialized"}, {"params", Json::object()}});
                            ready_ = true;
                            Emit({{"event", "ready"}});
                        } else
                            Emit(std::move(frame));
                    }
                } else {
                    stderrText.append(data.data(), n);
                    if (stderrText.size() > 16384)
                        stderrText.erase(0, stderrText.size() - 16384);
                }
            }
        };
        read(outRead, true);
        read(errRead, false);
        if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
            read(outRead, true);
            read(errRead, false);
            Emit({{"event", "error"},
                  {"category", IsAuthenticationFailure(MultiByteToWide(
                                   reinterpret_cast<const uint8_t *>(stderrText.data()), stderrText.size(), CP_UTF8))
                                   ? "authentication"
                                   : "connection"},
                  {"message", stderrText.find("Access denied") != std::string::npos ||
                                      stderrText.find("authentication") != std::string::npos
                                  ? u8"SSH 认证失败，请检查用户名和密码"
                              : stderrText.find("HOST KEY") != std::string::npos ||
                                      stderrText.find("host key") != std::string::npos
                                  ? u8"SSH 主机身份验证失败，连接已阻止；请重新核对服务器指纹"
                              : stderrText.find("SERIALCTL_CODEX_NOT_FOUND") != std::string::npos
                                  ? u8"SSH 可连接，但登录环境中找不到 Codex；请在服务器为该用户配置 PATH"
                                  : u8"SSH / Codex 服务已退出；请检查认证、服务器 Codex 配置和服务状态"}});
            break;
        }
        if (!ready_ && GetTickCount64() - started > 30000) {
            Emit({{"event", "error"}, {"message", u8"App Server 启动或协议握手超时（30 秒）"}});
            break;
        }
        if (heartbeatAt && GetTickCount64() - heartbeatAt > 15000) {
            Emit({{"event", "error"}, {"message", u8"Codex 服务 15 秒未响应健康检查，连接已关闭；不自动重发任务"}});
            break;
        }
        if (ready_ && !heartbeatAt && GetTickCount64() - lastRead > 45000) {
            // A harmless request is a liveness probe; no model invocation.
            Send({{"id", "serialctl-heartbeat"}, {"method", "config/read"}, {"params", {{"includeLayers", false}}}});
            heartbeatAt = GetTickCount64();
        }
        if (sending.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!writes_.empty()) {
                sending = std::move(writes_.front());
                writes_.pop_front();
                queuedBytes_ -= sending.size();
                sent = 0;
            }
        }
        if (!sending.empty()) {
            // <= 1 KiB per poll; CancelSynchronousIo in Shutdown interrupts a
            // broken/non-reading peer rather than leaving the Win32 UI blocked.
            DWORD n = 0;
            if (!WriteFile(inWrite, sending.data() + sent,
                           static_cast<DWORD>(std::min<size_t>(1024, sending.size() - sent)), &n, nullptr) ||
                !n)
                throw std::runtime_error("SSH stdin closed");
            sent += n;
            if (sent == sending.size())
                sending.clear();
        }
        WaitForSingleObject(process, 10);
    }
}
} // namespace serialctl::ai
