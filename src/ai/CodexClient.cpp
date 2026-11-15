#include "CodexClient.h"
#include "ChildProcess.h"
#include "PasswordPipe.h"
#include "PlatformConnection.h"
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
std::wstring WorkspaceName(const ServerSettings& s) {
    auto host=s.host;for(auto& ch:host)ch=static_cast<wchar_t>(towlower(ch));
    std::wstring endpoint = s.localMode ? L"local" : (host+L":"+std::to_wstring(s.port)+L"/"+s.username);
    if (s.provider == AiProvider::Antigravity) endpoint = L"agy:" + endpoint;
    // Registry value names are case-insensitive; encode account bytes so Linux
    // and linux cannot share a workspace entry accidentally.
    constexpr wchar_t digits[]=L"0123456789abcdef";
    std::wstring name;
    for(const auto ch:endpoint)for(int shift:{12,8,4,0})name+=digits[(static_cast<unsigned>(ch)>>shift)&15];
    return name;
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
bool FileExists(const std::wstring &path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}
std::wstring FindOnPath(const wchar_t *name) {
    wchar_t buf[32768]{};
    const DWORD n = SearchPathW(nullptr, name, nullptr, 32768, buf, nullptr);
    return n > 0 && n < 32768 ? std::wstring(buf, n) : std::wstring();
}
std::wstring ExpandEnv(const wchar_t *expr) {
    wchar_t buf[32768]{};
    const DWORD n = ExpandEnvironmentStringsW(expr, buf, 32768);
    return n > 0 && n < 32768 ? std::wstring(buf) : std::wstring();
}
bool SupportsAcpProcessPerSession(const std::wstring &cmdExe, const std::wstring &cli, bool viaCmd) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0))
        return false;
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nulIn != INVALID_HANDLE_VALUE ? nulIn : nullptr;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    PROCESS_INFORMATION pi{};
    const std::wstring app = viaCmd ? cmdExe : cli;
    std::wstring cmd = viaCmd ? (QuoteCommandLineArgument(cmdExe) + L" /d /c \"" + QuoteCommandLineArgument(cli) +
                                 L" acp --help\"")
                              : (QuoteCommandLineArgument(cli) + L" acp --help");
    const BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si, &pi);
    CloseHandle(outWrite);
    if (nulIn != INVALID_HANDLE_VALUE)
        CloseHandle(nulIn);
    if (!ok) {
        CloseHandle(outRead);
        return false;
    }
    std::string output;
    const auto deadline = GetTickCount64() + 3000;
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(outRead, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            char buf[2048];
            DWORD readBytes = 0;
            if (ReadFile(outRead, buf, sizeof(buf), &readBytes, nullptr) && readBytes > 0) {
                if (output.size() < 32768)
                    output.append(buf, readBytes);
                continue;
            }
        }
        const DWORD waitResult = WaitForSingleObject(pi.hProcess, 25);
        if (waitResult == WAIT_OBJECT_0) {
            char buf[2048];
            DWORD readBytes = 0;
            while (PeekNamedPipe(outRead, nullptr, 0, nullptr, &available, nullptr) && available > 0 &&
                   ReadFile(outRead, buf, sizeof(buf), &readBytes, nullptr) && readBytes > 0) {
                if (output.size() < 32768)
                    output.append(buf, readBytes);
            }
            break;
        }
        if (GetTickCount64() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 200);
            break;
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(outRead);
    return output.find("--process-per-session") != std::string::npos;
}
bool ResolveLocalCli(AiProvider provider, std::wstring &application, std::wstring &command, std::wstring &error) {
    if (provider == AiProvider::Codex) {
        std::wstring exe = FindOnPath(L"codex.exe");
        if (exe.empty()) {
            const auto fallback = ExpandEnv(L"%LOCALAPPDATA%\\OpenAI\\Codex\\bin\\codex.exe");
            if (FileExists(fallback)) exe = fallback;
        }
        if (exe.empty()) {
            error = L"本机未找到 codex.exe；请安装 Codex CLI 或将其添加到 PATH";
            return false;
        }
        application = exe;
        command = QuoteCommandLineArgument(exe) + L" app-server --listen stdio://";
        return true;
    }
    std::wstring cli = FindOnPath(L"agy.exe");
    bool isBatch = false;
    if (cli.empty()) {
        for (const wchar_t *candidate : {L"agy.cmd", L"agy.bat"}) {
            cli = FindOnPath(candidate);
            if (!cli.empty()) { isBatch = true; break; }
        }
    }
    if (cli.empty()) {
        const auto localBin = ExpandEnv(L"%USERPROFILE%\\.local\\bin\\agy.exe");
        if (FileExists(localBin)) cli = localBin;
    }
    if (cli.empty()) {
        const auto progCmd = ExpandEnv(L"%LOCALAPPDATA%\\Programs\\Antigravity\\bin\\agy.cmd");
        if (FileExists(progCmd)) { cli = progCmd; isBatch = true; }
    }
    if (cli.empty()) {
        error = L"本机未找到 agy（Antigravity CLI）；请安装后添加到 PATH，或改用远程 SSH 模式";
        return false;
    }
    if (isBatch) {
        const auto cmdExe = FindOnPath(L"cmd.exe");
        if (cmdExe.empty()) {
            error = L"本机未找到 cmd.exe";
            return false;
        }
        const std::wstring flags = SupportsAcpProcessPerSession(cmdExe, cli, true)
                                       ? L" acp --process-per-session --dangerously-skip-permissions"
                                       : L" acp --dangerously-skip-permissions";
        application = cmdExe;
        command = QuoteCommandLineArgument(cmdExe) + L" /d /c \"" + QuoteCommandLineArgument(cli) + flags + L"\"";
        return true;
    }
    const std::wstring flags = SupportsAcpProcessPerSession({}, cli, false)
                                   ? L" acp --process-per-session --dangerously-skip-permissions"
                                   : L" acp --dangerously-skip-permissions";
    application = cli;
    command = QuoteCommandLineArgument(cli) + flags;
    return true;
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
    if (s.localMode) {
        if (s.workingDirectory.size() > 4096 ||
            s.workingDirectory.find_first_of(L"\r\n") != std::wstring::npos ||
            s.workingDirectory.find(L'\0') != std::wstring::npos) {
            error = L"本地工作目录路径无效";
            return false;
        }
        return true;
    }
    unsigned actual=0;
    if(s.platformMode && (!PlatformSshPort(s.employeeNumber,actual) || actual!=s.port)) {
        error=L"请输入有效员工工号（1–55535）";return false;
    }
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
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SettingsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        s.platformMode=true;return s;
    }
    s.host = ReadString(key, L"Host");
    s.employeeNumber=ReadString(key,L"EmployeeNumber");
    s.platformMode=!s.employeeNumber.empty();
    s.username = ReadString(key, L"Username");
    if (s.username.empty()) s.username = L"linux";
    s.workingDirectory = ReadString(key, L"WorkingDirectory");
    if (s.workingDirectory.empty()) s.workingDirectory = L"/home/linux/workspace";
    DWORD port = 22, size = sizeof(port), type = REG_DWORD;
    RegQueryValueExW(key, L"Port", nullptr, &type, reinterpret_cast<BYTE *>(&port), &size);
    s.port = port > 0 && port <= 65535 ? port : 22;
    DWORD provider = 0;
    size = sizeof(provider); type = REG_DWORD;
    if (RegQueryValueExW(key, L"Provider", nullptr, &type, reinterpret_cast<BYTE *>(&provider), &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
        s.provider = provider == 1 ? AiProvider::Antigravity : AiProvider::Codex;
    DWORD localMode = 0;
    size = sizeof(localMode); type = REG_DWORD;
    if (RegQueryValueExW(key, L"LocalMode", nullptr, &type, reinterpret_cast<BYTE *>(&localMode), &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
        s.localMode = localMode != 0;
    if(s.platformMode && !PlatformSshPort(s.employeeNumber,s.port)) {s.platformMode=false;s.employeeNumber.clear();}
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
std::wstring WorkspaceForServer(const ServerSettings& s) {
    HKEY key=nullptr;
    std::wstring path;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\SerialCtl\\AI\\Workspaces",0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS) {
        path=ReadString(key,WorkspaceName(s).c_str());RegCloseKey(key);
    }
    return path.empty()?L"/home/linux/workspace":path;
}
WorkspaceSettings LoadWorkspaceSettings(const ServerSettings *activeServer) {
    WorkspaceSettings ws;
    ws.remoteDirectory = activeServer && !activeServer->localMode && !activeServer->host.empty()
                             ? WorkspaceForServer(*activeServer)
                             : L"/home/linux/workspace";

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SerialCtl\\AI\\Workspace", 0, KEY_QUERY_VALUE, &key) ==
        ERROR_SUCCESS) {
        DWORD isLocal = 0, readOnly = 0, type = REG_DWORD, size = sizeof(isLocal);
        if (RegQueryValueExW(key, L"IsLocal", nullptr, &type, reinterpret_cast<BYTE *>(&isLocal), &size) ==
                ERROR_SUCCESS &&
            type == REG_DWORD) {
            ws.isLocal = isLocal != 0;
        }
        size = sizeof(readOnly);
        type = REG_DWORD;
        if (RegQueryValueExW(key, L"LocalReadOnly", nullptr, &type, reinterpret_cast<BYTE *>(&readOnly), &size) ==
                ERROR_SUCCESS &&
            type == REG_DWORD) {
            ws.localReadOnly = readOnly != 0;
        }
        ws.localDirectory = ReadString(key, L"LocalDirectory");
        const auto savedRemote = ReadString(key, L"RemoteDirectory");
        if (!savedRemote.empty() && (!activeServer || activeServer->localMode || activeServer->host.empty())) {
            ws.remoteDirectory = savedRemote;
        }
        const auto sshSid = ReadString(key, L"RemoteSshSessionId");
        ws.remoteSshSessionId = WideToMultiByte(sshSid, CP_UTF8);
        RegCloseKey(key);
        return ws;
    }

    // Legacy migration: if previous LocalMode saved a Windows workingDirectory, preserve it once.
    const auto legacy = LoadServerSettings();
    if (legacy.localMode && !legacy.workingDirectory.empty() && legacy.workingDirectory.front() != L'/') {
        ws.isLocal = true;
        ws.localDirectory = legacy.workingDirectory;
    }
    return ws;
}
bool SaveWorkspaceSettings(const WorkspaceSettings &ws, const ServerSettings *activeServer, std::wstring &error) {
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SerialCtl\\AI\\Workspace", 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result == ERROR_SUCCESS) {
        auto setSz = [&](const wchar_t *name, const std::wstring &text) {
            if (result == ERROR_SUCCESS)
                result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(text.c_str()),
                                        static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
        };
        DWORD isLocal = ws.isLocal ? 1u : 0u;
        DWORD readOnly = ws.localReadOnly ? 1u : 0u;
        result = RegSetValueExW(key, L"IsLocal", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&isLocal), sizeof(isLocal));
        if (result == ERROR_SUCCESS)
            result = RegSetValueExW(key, L"LocalReadOnly", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&readOnly),
                                    sizeof(readOnly));
        setSz(L"LocalDirectory", ws.localDirectory);
        setSz(L"RemoteDirectory", ws.remoteDirectory);
        setSz(L"RemoteSshSessionId",
              MultiByteToWide(reinterpret_cast<const uint8_t *>(ws.remoteSshSessionId.data()),
                              ws.remoteSshSessionId.size(), CP_UTF8));
        RegCloseKey(key);
    }
    if (result == ERROR_SUCCESS && activeServer && !activeServer->localMode && !activeServer->host.empty() &&
        !ws.remoteDirectory.empty() && ws.remoteDirectory.front() == L'/') {
        HKEY spaces = nullptr;
        result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SerialCtl\\AI\\Workspaces", 0, nullptr, 0,
                                 KEY_SET_VALUE, nullptr, &spaces, nullptr);
        if (result == ERROR_SUCCESS) {
            result = RegSetValueExW(spaces, WorkspaceName(*activeServer).c_str(), 0, REG_SZ,
                                    reinterpret_cast<const BYTE *>(ws.remoteDirectory.c_str()),
                                    static_cast<DWORD>((ws.remoteDirectory.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(spaces);
        }
    }
    if (result != ERROR_SUCCESS) {
        error = L"保存工作空间配置失败：" + Win32ErrorMessage(result);
        return false;
    }
    return true;
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
        set(L"EmployeeNumber",s.platformMode?s.employeeNumber:L"");
        DWORD port = s.port;
        if (result == ERROR_SUCCESS)
            result = RegSetValueExW(key, L"Port", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&port), sizeof(port));
        DWORD provider = s.provider == AiProvider::Antigravity ? 1u : 0u;
        if (result == ERROR_SUCCESS)
            result = RegSetValueExW(key, L"Provider", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&provider), sizeof(provider));
        DWORD localMode = s.localMode ? 1u : 0u;
        if (result == ERROR_SUCCESS)
            result = RegSetValueExW(key, L"LocalMode", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&localMode), sizeof(localMode));
        RegCloseKey(key);
    }
    if (result == ERROR_SUCCESS && !s.workingDirectory.empty()) {
        HKEY spaces=nullptr;
        result=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\SerialCtl\\AI\\Workspaces",0,nullptr,0,KEY_SET_VALUE,nullptr,&spaces,nullptr);
        if(result==ERROR_SUCCESS) {
            result=RegSetValueExW(spaces,WorkspaceName(s).c_str(),0,REG_SZ,reinterpret_cast<const BYTE*>(s.workingDirectory.c_str()),static_cast<DWORD>((s.workingDirectory.size()+1)*sizeof(wchar_t)));
            RegCloseKey(spaces);
        }
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
    std::wstring application, command, fingerprint;
    PasswordPipe password;
    const wchar_t *currentDir = nullptr;
    std::wstring localDir;
    if (settings.localMode) {
        Emit({{"event", "status"},
              {"message", settings.provider == AiProvider::Antigravity
                              ? u8"正在本机启动 Antigravity ACP…"
                              : u8"正在本机启动 Codex App Server…"}});
        if (!ResolveLocalCli(settings.provider, application, command, error)) {
            Emit({{"event", "error"}, {"message", WideToMultiByte(error, CP_UTF8)}});
            return;
        }
        if (!settings.workingDirectory.empty()) {
            const DWORD attrs = GetFileAttributesW(settings.workingDirectory.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                localDir = settings.workingDirectory;
                currentDir = localDir.c_str();
            }
        }
    } else {
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
        Emit({{"event", "status"},
              {"message", settings.provider == AiProvider::Antigravity
                              ? u8"SSH 已核验；正在检测 Antigravity (agy) 并启动 ACP…"
                              : u8"SSH 已核验；正在检测 Codex 并启动 App Server…"}});
        if (!password.Open(settings.password, error)) {
            Emit({{"event", "error"}, {"message", u8"安全密码通道创建失败"}});
            return;
        }
        application = plink;
        command = QuoteCommandLineArgument(plink) +
                  L" -batch -ssh -T -no-antispoof -no-sanitise-stdout -no-sanitise-stderr -P " +
                  std::to_wstring(settings.port) + L" -l " + QuoteCommandLineArgument(settings.username) +
                  L" -pwfile " + QuoteCommandLineArgument(password.Name());
        if (!fingerprint.empty())
            command += L" -hostkey " + QuoteCommandLineArgument(fingerprint);
        if (settings.provider == AiProvider::Antigravity) {
            const unsigned localPort = settings.mcpPort ? settings.mcpPort : 7080;
            const unsigned remotePort = settings.remoteMcpPort ? settings.remoteMcpPort : localPort;
            command += L" -R " + QuoteCommandLineArgument(
                L"127.0.0.1:" + std::to_wstring(remotePort) + L":127.0.0.1:" + std::to_wstring(localPort));
        }
        // Constant command; no host/user/model text is interpolated into a shell.
        const wchar_t *remoteShell =
            settings.provider == AiProvider::Antigravity
                ? L"sh -lc 'if [ -r \"$HOME/.config/serialctl/codex-env.sh\" ]; then "
                  L". \"$HOME/.config/serialctl/codex-env.sh\" 1>&2; fi; "
                  L"export PATH=\"$HOME/.local/bin:$PATH\"; "
                  L"command -v agy >/dev/null 2>&1 || { echo SERIALCTL_AGY_NOT_FOUND >&2; exit 127; }; "
                  L"if agy acp --help </dev/null 2>&1 | grep -q -- --process-per-session; then "
                  L"exec agy acp --process-per-session --dangerously-skip-permissions; "
                  L"else exec agy acp --dangerously-skip-permissions; fi'"
                : L"sh -lc 'if [ -r \"$HOME/.config/serialctl/codex-env.sh\" ]; then "
                  L". \"$HOME/.config/serialctl/codex-env.sh\" 1>&2; fi; "
                  L"command -v codex >/dev/null 2>&1 || { echo SERIALCTL_CODEX_NOT_FOUND "
                  L">&2; exit 127; }; exec codex app-server --listen stdio://'";
        command += L" " + QuoteCommandLineArgument(settings.host) + L" " + QuoteCommandLineArgument(remoteShell);
    }
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr,
           errWrite = nullptr;
    HANDLE process = nullptr, processThread = nullptr, job = nullptr;
    struct Cleanup {
        HANDLE &a, &b, &c, &d, &e, &f, &process, &thread, &job;
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
            Close(job);
        }
    } cleanup{inRead, inWrite, outRead, outWrite, errRead, errWrite, process, processThread, job};
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0) ||
        !CreatePipe(&errRead, &errWrite, &sa, 0) || !SetHandleInheritable(inWrite, false, L"AI 输入", error) ||
        !SetHandleInheritable(outRead, false, L"AI 输出", error) ||
        !SetHandleInheritable(errRead, false, L"AI 错误输出", error)) {
        Emit({{"event", "error"}, {"message", u8"创建 AI stdio 通道失败"}});
        return;
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    RestrictedProcessStartup startup;
    if (!startup.Initialize(inRead, outWrite, errWrite, {}, error)) {
        Emit({{"event", "error"}, {"message", u8"限制 AI 子进程继承失败"}});
        return;
    }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(application.c_str(), buffer.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, currentDir, startup.StartupInfo(),
                        &pi)) {
        Emit({{"event", "error"},
              {"message", settings.localMode ? u8"无法启动本机 AI 进程" : u8"无法启动 SSH App Server 进程"}});
        return;
    }
    process = pi.hProcess;
    processThread = pi.hThread;
    job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, process)) {
            // Windows 7 rejects assigning a process into a second non-nested Job Object.
            CloseHandle(job);
            job = nullptr;
        }
    }
    Close(inRead);
    Close(outWrite);
    Close(errWrite);
    if (settings.provider == AiProvider::Antigravity) {
        Send({{"jsonrpc", "2.0"},
              {"id", 0},
              {"method", "initialize"},
              {"params",
               {{"protocolVersion", 1},
                {"clientCapabilities", Json::object()},
                {"clientInfo", {{"name", "serialctl_ai"}, {"version", "1.0"}}}}}});
    } else {
        Send({{"id", 0},
              {"method", "initialize"},
              {"params",
               {{"clientInfo", {{"name", "serialctl_ai"}, {"title", "SerialCtl AI"}, {"version", "1.0"}}},
                {"capabilities", {{"experimentalApi", true}}}}}});
    }
    JsonLines frames;
    std::string stderrText;
    bool tunnelRefusedNotified = false;
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
                            bool acpLoadSession = false;
                            bool acpSessionList = false;
                            bool acpSessionResume = false;
                            if (settings.provider == AiProvider::Antigravity && frame.contains("result") &&
                                frame["result"].is_object()) {
                                const auto &res = frame["result"];
                                if (res.contains("agentCapabilities") && res["agentCapabilities"].is_object()) {
                                    const auto &caps = res["agentCapabilities"];
                                    acpLoadSession = caps.value("loadSession", false);
                                    if (caps.contains("sessionCapabilities") && caps["sessionCapabilities"].is_object()) {
                                        const auto &sc = caps["sessionCapabilities"];
                                        acpSessionList = sc.contains("list") && !sc["list"].is_null();
                                        acpSessionResume = sc.contains("resume") && !sc["resume"].is_null();
                                    }
                                    if (caps.contains("mcpCapabilities") && caps["mcpCapabilities"].is_object()) {
                                        const auto &mcpCaps = caps["mcpCapabilities"];
                                        if (mcpCaps.contains("http") && mcpCaps["http"].is_boolean() &&
                                            !mcpCaps["http"].get<bool>()) {
                                            Emit({{"event", "status"},
                                                  {"message",
                                                   u8"警告：当前 Antigravity Agent 声明不支持 HTTP MCP（mcpCapabilities.http=false），将无法使用本机工作空间或串口工具"}});
                                        }
                                    }
                                }
                            }
                            if (settings.provider == AiProvider::Codex)
                                Send({{"method", "initialized"}, {"params", Json::object()}});
                            ready_ = true;
                            Emit({{"event", "ready"},
                                  {"acpLoadSession", acpLoadSession},
                                  {"acpSessionList", acpSessionList},
                                  {"acpSessionResume", acpSessionResume}});
                        } else
                            Emit(std::move(frame));
                    }
                } else {
                    stderrText.append(data.data(), n);
                    if (stderrText.size() > 16384)
                        stderrText.erase(0, stderrText.size() - 16384);
                    std::string lowerStderr = stderrText;
                    for (char &ch : lowerStderr) {
                        if (ch >= 'A' && ch <= 'Z')
                            ch = static_cast<char>(ch - 'A' + 'a');
                    }
                    const bool hasForwardingError =
                        lowerStderr.find("forwarding") != std::string::npos &&
                        (lowerStderr.find("refused") != std::string::npos ||
                         lowerStderr.find("failed") != std::string::npos ||
                         lowerStderr.find("prohibited") != std::string::npos ||
                         lowerStderr.find("rejected") != std::string::npos ||
                         lowerStderr.find("denied") != std::string::npos);
                    if (!tunnelRefusedNotified && settings.provider == AiProvider::Antigravity && !settings.localMode &&
                        hasForwardingError) {
                        tunnelRefusedNotified = true;
                        Emit({{"event", "status"},
                              {"message", u8"警告：SSH 服务端拒绝或禁用了反向端口转发（AllowTcpForwarding），远程 Antigravity 将无法调用本机工作空间或串口 MCP"}});
                    }
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
                              : stderrText.find("SERIALCTL_AGY_NOT_FOUND") != std::string::npos
                                  ? u8"SSH 可连接，但登录环境中找不到 agy（Antigravity CLI）；请在服务器安装 agy 或配置 PATH"
                                  : settings.localMode
                                        ? u8"本机 AI 服务已退出；请检查本机 CLI 安装、登录状态或工作目录"
                                        : u8"SSH / AI 服务已退出；请检查认证、服务器配置和服务状态"}});
            break;
        }
        if (!ready_ && GetTickCount64() - started > 30000) {
            Emit({{"event", "error"}, {"message", u8"AI 服务启动或协议握手超时（30 秒）"}});
            break;
        }
        if (heartbeatAt && GetTickCount64() - heartbeatAt > 15000) {
            Emit({{"event", "error"}, {"message", u8"Codex 服务 15 秒未响应健康检查，连接已关闭；不自动重发任务"}});
            break;
        }
        if (settings.provider == AiProvider::Codex && ready_ && !heartbeatAt && GetTickCount64() - lastRead > 45000) {
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
            // <= 4 KiB per poll (one default pipe buffer); CancelSynchronousIo in Shutdown interrupts a
            // broken/non-reading peer rather than leaving the Win32 UI blocked.
            DWORD n = 0;
            if (!WriteFile(inWrite, sending.data() + sent,
                           static_cast<DWORD>(std::min<size_t>(4096, sending.size() - sent)), &n, nullptr) ||
                !n)
                throw std::runtime_error("SSH stdin closed");
            sent += n;
            if (sent == sending.size())
                sending.clear();
        }
        // Only idle-wait when nothing is left to send; pending data is written on the next pass.
        if (sending.empty()) WaitForSingleObject(process, 10);
    }
}
} // namespace serialctl::ai
