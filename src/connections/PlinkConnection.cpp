#include "PlinkConnection.h"
#include "ChildProcess.h"
#include "PuttyHostKey.h"
#include "Win32Helpers.h"

#include <array>

namespace serialctl {

PlinkConnection::PlinkConnection(
    std::wstring host,
    std::uint16_t port,
    std::wstring username,
    std::wstring password,
    int terminalColumns,
    int terminalRows, HostKeyConfirmation confirm)
    : host_(std::move(host)),
      port_(port),
      username_(std::move(username)),
      password_(std::move(password)),
      confirmHostKey_(std::move(confirm)),
      terminalColumns_(std::max(20, terminalColumns)),
      terminalRows_(std::max(4, terminalRows)) {}

PlinkConnection::~PlinkConnection() {
    Stop();
}

bool PlinkConnection::Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) {
    stopping_ = false;
    const std::wstring plink = FindPlink();
    if (plink.empty()) {
        error = L"未找到 plink.exe。请将官方 PuTTY plink.exe 放在 serialctl.exe 同一目录。";
        return false;
    }
    if (!ResolvePuttyHostKey(plink, host_, port_, username_, password_,
            hostKey_, error, &cancelStarting_, confirmHostKey_)) {
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE outputWrite = nullptr;
    HANDLE inputRead = nullptr;
    if (!CreatePipe(&outputRead_, &outputWrite, &security, 0) ||
        !CreatePipe(&inputRead, &inputWrite_, &security, 0)) {
        error = L"创建 SSH 管道失败：" + Win32ErrorMessage();
        if (outputRead_) CloseHandle(outputRead_);
        if (outputWrite) CloseHandle(outputWrite);
        if (inputRead) CloseHandle(inputRead);
        if (inputWrite_) CloseHandle(inputWrite_);
        outputRead_ = inputWrite_ = nullptr;
        return false;
    }
    if (!SetHandleInheritable(outputRead_, false,
            L"设置 SSH 输出管道读取端继承属性失败", error) ||
        !SetHandleInheritable(inputWrite_, false,
            L"设置 SSH 输入管道写入端继承属性失败", error)) {
        CloseHandle(outputRead_);
        CloseHandle(outputWrite);
        CloseHandle(inputRead);
        CloseHandle(inputWrite_);
        outputRead_ = inputWrite_ = nullptr;
        return false;
    }

    if (!CreateResizeChannel(error)) {
        CloseHandle(outputRead_);
        CloseHandle(outputWrite);
        CloseHandle(inputRead);
        CloseHandle(inputWrite_);
        outputRead_ = inputWrite_ = nullptr;
        return false;
    }

    const bool hasPuttySession = CreatePuttySession();
    std::wstring command = QuoteCommandLineArgument(plink);
    if (hasPuttySession) command += L" -load " + QuoteCommandLineArgument(puttySessionName_);
    command += L" -batch -ssh -t -no-antispoof -no-sanitise-stdout -no-sanitise-stderr -P " +
        std::to_wstring(port_) +
        L" -l " + QuoteCommandLineArgument(username_);
    command += L" -serialctl-resize-event " +
        std::to_wstring(reinterpret_cast<UINT_PTR>(resizeEvent_)) +
        L" -serialctl-resize-map " +
        std::to_wstring(reinterpret_cast<UINT_PTR>(resizeMapping_));
    if (!password_.empty()) {
        command += L" -pw " + QuoteCommandLineArgument(password_);
    }
    if (!hostKey_.empty())
        command += L" -hostkey " + QuoteCommandLineArgument(hostKey_);
    command += L" " + QuoteCommandLineArgument(host_);
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');

    RestrictedProcessStartup startup;
    if (!startup.Initialize(inputRead, outputWrite, outputWrite,
            {resizeEvent_, resizeMapping_}, error)) {
        DeletePuttySession();
        CloseResizeChannel();
        CloseHandle(inputRead);
        CloseHandle(outputWrite);
        CloseHandle(outputRead_);
        CloseHandle(inputWrite_);
        outputRead_ = inputWrite_ = nullptr;
        return false;
    }
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(
        plink.c_str(),
        commandBuffer.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        nullptr,
        startup.StartupInfo(),
        &processInfo);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(inputRead);
    CloseHandle(outputWrite);

    if (!created) {
        DeletePuttySession();
        CloseResizeChannel();
        error = L"启动 plink.exe 失败：" + Win32ErrorMessage(createError);
        CloseHandle(outputRead_);
        CloseHandle(inputWrite_);
        outputRead_ = inputWrite_ = nullptr;
        return false;
    }

    process_ = processInfo.hProcess;
    processThread_ = processInfo.hThread;
    onData_ = std::move(onData);
    onStatus_ = std::move(onStatus);
    readThread_ = std::thread(&PlinkConnection::ReadLoop, this);
    if (onStatus_) {
        onStatus_(L"SSH 已启动：" + host_ + L":" + std::to_wstring(port_), false);
    }
    return true;
}

void PlinkConnection::Stop() {
    stopping_ = true;
    if (inputWrite_) {
        CloseHandle(inputWrite_);
        inputWrite_ = nullptr;
    }
    if (process_) {
        if (WaitForSingleObject(process_, 1000) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 1000);
        }
    }
    if (outputRead_) {
        CloseHandle(outputRead_);
        outputRead_ = nullptr;
    }
    if (readThread_.joinable()) {
        readThread_.join();
    }
    if (processThread_) {
        CloseHandle(processThread_);
        processThread_ = nullptr;
    }
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    CloseResizeChannel();
    DeletePuttySession();
    onData_ = {};
    onStatus_ = {};
}

bool PlinkConnection::Send(const Bytes& data, std::wstring& error) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (!IsConnected() || !inputWrite_) {
        error = L"SSH 尚未连接";
        return false;
    }
    DWORD written = 0;
    if (!WriteFile(inputWrite_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) ||
        written != data.size()) {
        error = L"SSH 写入失败：" + Win32ErrorMessage();
        return false;
    }
    return true;
}

bool PlinkConnection::IsConnected() const {
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

void PlinkConnection::ResizeTerminal(int columns, int rows) {
    terminalColumns_ = std::max(20, columns);
    terminalRows_ = std::max(4, rows);
    if (!resizeState_ || !resizeEvent_) return;
    InterlockedExchange(&resizeState_[0], terminalColumns_);
    InterlockedExchange(&resizeState_[1], terminalRows_);
    SetEvent(resizeEvent_);
}

void PlinkConnection::ReadLoop() {
    std::array<std::uint8_t, 4096> buffer{};
    while (!stopping_) {
        DWORD read = 0;
        if (!ReadFile(outputRead_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) {
            break;
        }
        if (onData_) {
            onData_(Bytes(buffer.begin(), buffer.begin() + read));
        }
    }
    if (!stopping_ && onStatus_) {
        DWORD exitCode = 0;
        if (process_) {
            GetExitCodeProcess(process_, &exitCode);
        }
        onStatus_(L"SSH 进程已结束，退出码 " + std::to_wstring(exitCode), exitCode != 0);
    }
}

std::wstring PlinkConnection::FindPlink() {
    std::array<wchar_t, MAX_PATH> modulePath{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) {
        return {};
    }
    std::wstring directory(modulePath.data(), length);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        directory.resize(slash + 1);
    } else {
        directory.clear();
    }
    const std::wstring candidate = directory + L"plink.exe";
    return GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES ? candidate : std::wstring{};
}

bool PlinkConnection::CreateResizeChannel(std::wstring& error) {
    CloseResizeChannel();
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    resizeEvent_ = CreateEventW(&security, FALSE, FALSE, nullptr);
    resizeMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security,
        PAGE_READWRITE, 0, sizeof(LONG) * 2, nullptr);
    if (resizeEvent_ && resizeMapping_) {
        resizeState_ = static_cast<volatile LONG*>(MapViewOfFile(
            resizeMapping_, FILE_MAP_WRITE, 0, 0, sizeof(LONG) * 2));
    }
    if (!resizeEvent_ || !resizeMapping_ || !resizeState_) {
        error = L"创建 SSH 终端尺寸通道失败：" + Win32ErrorMessage();
        CloseResizeChannel();
        return false;
    }
    InterlockedExchange(&resizeState_[0], terminalColumns_);
    InterlockedExchange(&resizeState_[1], terminalRows_);
    return true;
}

void PlinkConnection::CloseResizeChannel() {
    if (resizeState_) {
        UnmapViewOfFile(const_cast<LONG*>(resizeState_));
        resizeState_ = nullptr;
    }
    if (resizeMapping_) {
        CloseHandle(resizeMapping_);
        resizeMapping_ = nullptr;
    }
    if (resizeEvent_) {
        CloseHandle(resizeEvent_);
        resizeEvent_ = nullptr;
    }
}

bool PlinkConnection::CreatePuttySession() {
    puttySessionName_ = L"SerialCtl-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount()) + L"-" + std::to_wstring(reinterpret_cast<UINT_PTR>(this));
    const std::wstring path = L"Software\\SimonTatham\\PuTTY\\Sessions\\" + puttySessionName_;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        puttySessionName_.clear();
        return false;
    }
    const DWORD width = static_cast<DWORD>(terminalColumns_);
    const DWORD height = static_cast<DWORD>(terminalRows_);
    static const wchar_t terminalType[] = L"xterm-256color";
    const bool success =
        RegSetValueExW(key, L"TermWidth", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&width), sizeof(width)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"TermHeight", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&height), sizeof(height)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"TerminalType", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(terminalType), sizeof(terminalType)) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!success) DeletePuttySession();
    return success;
}

void PlinkConnection::DeletePuttySession() {
    if (puttySessionName_.empty()) return;
    const std::wstring path = L"Software\\SimonTatham\\PuTTY\\Sessions\\" + puttySessionName_;
    RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
    puttySessionName_.clear();
}

} // namespace serialctl
