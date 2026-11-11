#include "TftpFileSource.h"
#include "ChildProcess.h"
#include "PasswordPipe.h"
#include "PuttyHostKey.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <system_error>
#include <utility>

namespace serialctl {

// Helper to escape single quotes in POSIX paths: ' -> '\''
static std::wstring EscapePosixSingleQuotes(const std::wstring& path) {
    std::wstring out;
    for (wchar_t c : path) {
        if (c == L'\'') {
            out += L"'\\''";
        } else {
            out += c;
        }
    }
    return out;
}

namespace {
struct SourceHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~SourceHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    SourceHandle() = default;
    SourceHandle(const SourceHandle&) = delete;
    SourceHandle& operator=(const SourceHandle&) = delete;
};

uint64_t FileSize(const BY_HANDLE_FILE_INFORMATION& snapshot) {
    return (static_cast<uint64_t>(snapshot.nFileSizeHigh) << 32) | snapshot.nFileSizeLow;
}

bool ReadSnapshot(HANDLE file, BY_HANDLE_FILE_INFORMATION& snapshot) {
    return GetFileType(file) == FILE_TYPE_DISK &&
        GetFileInformationByHandle(file, &snapshot) &&
        !(snapshot.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

bool MatchesSnapshot(HANDLE file, const BY_HANDLE_FILE_INFORMATION& expected) {
    BY_HANDLE_FILE_INFORMATION actual{};
    return ReadSnapshot(file, actual) &&
        actual.dwVolumeSerialNumber == expected.dwVolumeSerialNumber &&
        actual.nFileIndexHigh == expected.nFileIndexHigh && actual.nFileIndexLow == expected.nFileIndexLow &&
        FileSize(actual) == FileSize(expected) &&
        CompareFileTime(&actual.ftLastWriteTime, &expected.ftLastWriteTime) == 0;
}

std::wstring TrimWhitespace(const std::wstring& value) {
    const size_t start = value.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return {};
    const size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(start, end - start + 1);
}

bool IsNotFoundWin32Error(DWORD err) {
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

bool OpenLocalVerifiedDirectoryChain(const std::filesystem::path& dir,
                                     std::vector<std::unique_ptr<SourceHandle>>& directories,
                                     bool& notFound) {
    notFound = false;
    const auto& raw = dir.native();
    if (!dir.is_absolute() || raw.find(L'\0') != std::wstring::npos ||
        raw.rfind(L"\\\\.\\", 0) == 0 ||
        dir.relative_path().native().find(L':') != std::wstring::npos) {
        return false;
    }
    for (const auto& part : dir.relative_path()) {
        const std::wstring name = part.native();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ') {
            return false;
        }
    }
    if (dir.relative_path().empty()) {
        auto handle = std::make_unique<SourceHandle>();
        handle->value = CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
        BY_HANDLE_FILE_INFORMATION directory{};
        if (handle->value == INVALID_HANDLE_VALUE) {
            notFound = IsNotFoundWin32Error(GetLastError());
            return false;
        }
        if (!GetFileInformationByHandle(handle->value, &directory) ||
            !(directory.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (directory.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return false;
        }
        directories.push_back(std::move(handle));
        return true;
    }

    auto current = dir.root_path();
    for (const auto& part : dir.relative_path()) {
        current /= part;
        auto handle = std::make_unique<SourceHandle>();
        handle->value = CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
        BY_HANDLE_FILE_INFORMATION directory{};
        if (handle->value == INVALID_HANDLE_VALUE) {
            notFound = IsNotFoundWin32Error(GetLastError());
            return false;
        }
        if (!GetFileInformationByHandle(handle->value, &directory) ||
            !(directory.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (directory.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return false;
        }
        directories.push_back(std::move(handle));
    }
    return true;
}

HANDLE OpenLocalSource(const std::wstring& path, bool* notFound = nullptr) {
    namespace fs = std::filesystem;
    if (notFound) *notFound = false;
    const fs::path file(path);
    const std::wstring filename = file.filename().native();
    if (!file.is_absolute() || filename.empty() || filename == L"." || filename == L".." ||
        filename.back() == L'.' || filename.back() == L' ' ||
        path.find(L'\0') != std::wstring::npos ||
        path.rfind(L"\\\\.\\", 0) == 0 ||
        file.relative_path().native().find(L':') != std::wstring::npos) {
        return INVALID_HANDLE_VALUE;
    }
    // Open each ancestor without following a reparse point, and keep it open
    // against replacement until the final handle has been acquired.
    std::vector<std::unique_ptr<SourceHandle>> directories;
    bool ancestorNotFound = false;
    if (!OpenLocalVerifiedDirectoryChain(file.parent_path(), directories, ancestorNotFound)) {
        if (notFound) *notFound = ancestorNotFound;
        return INVALID_HANDLE_VALUE;
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE && notFound) {
        *notFound = IsNotFoundWin32Error(GetLastError());
    }
    return handle;
}
bool LaunchPlink(const SshRemoteConfig& config, const std::wstring& passwordPipe,
                 const std::wstring& remoteCommand, bool diagnostic,
                 HANDLE& output, HANDLE& process, std::wstring& error) {
    std::vector<wchar_t> module(32768);
    const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!count || count >= module.size()) {
        error = L"无法定位配套 plink.exe";
        return false;
    }
    const auto plink = std::filesystem::path(std::wstring(module.data(), count)).parent_path() / L"plink.exe";
    std::wstring command = QuoteCommandLineArgument(plink.native()) +
        L" -batch -ssh -T -no-antispoof -no-sanitise-stdout -P " + std::to_wstring(config.port) +
        L" -l " + QuoteCommandLineArgument(config.username);
    if (!passwordPipe.empty()) command += L" -pwfile " + QuoteCommandLineArgument(passwordPipe);
    if (!config.hostKey.empty()) command += L" -hostkey " + QuoteCommandLineArgument(config.hostKey);
    command += L" " + QuoteCommandLineArgument(config.host) + L" " + QuoteCommandLineArgument(remoteCommand);

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    SourceHandle nullDevice;
    nullDevice.value = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE reader = nullptr, writer = nullptr;
    if (nullDevice.value == INVALID_HANDLE_VALUE || !CreatePipe(&reader, &writer, &inherit, 65536)) {
        error = L"无法创建 plink 输入输出通道";
        return false;
    }
    SourceHandle readEnd, writeEnd;
    readEnd.value = reader;
    writeEnd.value = writer;
    RestrictedProcessStartup startup;
    if (!SetHandleInheritable(reader, false, L"plink 输出", error) ||
        !startup.Initialize(nullDevice.value, writer, diagnostic ? writer : nullDevice.value, {}, error))
        return false;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(plink.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, startup.StartupInfo(), &child)) {
        error = L"无法启动配套 plink.exe";
        return false;
    }
    CloseHandle(child.hThread);
    process = child.hProcess;
    output = std::exchange(readEnd.value, INVALID_HANDLE_VALUE);
    return true;
}

constexpr auto RemoteStat = L"LC_ALL=C stat -L -c '%s %d %i %y %z' -- ";
} // namespace

LocalTftpFileSource::LocalTftpFileSource(std::wstring path)
    : path_(std::move(path)) {
    info_.type = "local";
    info_.displayPath = path_;
    info_.status = "Failed";
    bool notFound = false;
    SourceHandle file;
    file.value = OpenLocalSource(path_, &notFound);
    if (file.value == INVALID_HANDLE_VALUE || !ReadSnapshot(file.value, snapshot_)) {
        info_.notFound = (file.value == INVALID_HANDLE_VALUE) && notFound;
        info_.error = info_.notFound ? L"本地文件不存在" : L"无法读取常规文件，或路径包含重解析点";
        return;
    }
    info_.size = FileSize(snapshot_);
    info_.sizeKnown = true;
    info_.status = "Ready";
}

LocalTftpFileSource::LocalTftpFileSource(std::wstring path, BY_HANDLE_FILE_INFORMATION snapshot, TftpSourceInfo info)
    : path_(std::move(path)), snapshot_(snapshot), info_(std::move(info)) {
}

LocalTftpFileSource::~LocalTftpFileSource() {
    Close();
}

TftpSourceInfo LocalTftpFileSource::Info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

std::shared_ptr<ITftpFileSource> LocalTftpFileSource::CreateSessionSource() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::shared_ptr<ITftpFileSource>(new LocalTftpFileSource(path_, snapshot_, info_));
}

bool LocalTftpFileSource::Open(std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        return true;
    }

    if (info_.status != "Ready") {
        error = info_.error;
        return false;
    }
    SourceHandle file;
    file.value = OpenLocalSource(path_);
    if (file.value == INVALID_HANDLE_VALUE || !MatchesSnapshot(file.value, snapshot_)) {
        error = L"源文件已变化、正在被写入或无法安全打开，请重新发布";
        return false;
    }
    file_ = std::exchange(file.value, INVALID_HANDLE_VALUE);
    return true;
}

void LocalTftpFileSource::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

bool LocalTftpFileSource::Read(uint64_t offset, void* buffer, size_t length, size_t& bytesRead, std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    bytesRead = 0;
    if (file_ == INVALID_HANDLE_VALUE) {
        error = L"File not open";
        return false;
    }

    if (!MatchesSnapshot(file_, snapshot_)) {
        error = L"源文件已变化或无法核对文件信息";
        return false;
    }
    const auto size = FileSize(snapshot_);
    if (offset >= size) return true;

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file_, li, nullptr, FILE_BEGIN)) {
        error = L"Seek failed";
        return false;
    }

    const auto toRead = std::min({static_cast<uint64_t>(length), size - offset, static_cast<uint64_t>(MAXDWORD)});
    DWORD readBytes = 0;
    if (!ReadFile(file_, buffer, static_cast<DWORD>(toRead), &readBytes, nullptr)) {
        error = L"Read failed";
        return false;
    }

    if (readBytes != toRead) {
        error = L"源文件在预期长度之前结束";
        return false;
    }
    bytesRead = readBytes;
    return true;
}

void LocalTftpFileSource::Cancel() {
    Close();
}

// ============================================================================
// RemoteSshTftpFileSource
// ============================================================================

namespace {
bool ValidateRemoteSshConfig(SshRemoteConfig& config, std::wstring& error) {
    if (config.remotePath.empty() || config.remotePath[0] != L'/' ||
        std::any_of(config.remotePath.begin(), config.remotePath.end(), [](wchar_t ch) { return ch < 32; })) {
        error = L"远程路径必须是以 '/' 开头的绝对 POSIX 路径，且不能包含控制字符";
        return false;
    }
    if (config.host.empty() || config.host.front() == L'-' || config.username.empty()) {
        error = L"SSH 主机或用户名无效";
        return false;
    }
    if (config.hostKey.empty()) config.hostKey = TrustedPuttyHostKey(config.host, config.port);
    if (config.hostKey.empty()) {
        error = L"远程发布需要先在 SerialCtl 中核验该 SSH 主机的身份";
        return false;
    }
    return true;
}

bool RunPlinkCommand(const SshRemoteConfig& config, const std::wstring& remoteCommand,
                     size_t maxOutputBytes, DWORD timeoutMs,
                     std::string& output, DWORD& exitCode, std::wstring& error) {
    output.clear();
    exitCode = 1;
    PasswordPipe password;
    if (!config.password.empty() && !password.Open(config.password, error)) return false;
    SourceHandle pipe, child;
    if (!LaunchPlink(config, password.Name(), remoteCommand, false, pipe.value, child.value, error)) {
        return false;
    }

    std::array<char, 4096> buffer{};
    const auto deadline = GetTickCount64() + timeoutMs;
    bool exited = false;
    bool overflow = false;
    while (GetTickCount64() < deadline) {
        if (!config.password.empty()) {
            password.Poll();
            if (password.Failed()) break;
        }
        DWORD available = 0, read = 0;
        if (PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(pipe.value, buffer.data(), want, &read, nullptr) || !read) break;
            if (output.size() + read > maxOutputBytes) {
                overflow = true;
                break;
            }
            output.append(buffer.data(), read);
            continue;
        }
        if (WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0) {
            // Drain again after observing exit, since it may have written after the peek.
            if (PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr) && available) continue;
            exited = true;
            break;
        }
        Sleep(10);
    }
    if (!exited) {
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, 1000);
        error = overflow ? L"SSH 返回数据超出限制" : L"SSH 检查超时";
        return false;
    }
    if (!GetExitCodeProcess(child.value, &exitCode)) {
        exitCode = 1;
        error = L"无法获取 SSH 检查结果";
        return false;
    }
    return true;
}
} // namespace

RemoteSshTftpFileSource::RemoteSshTftpFileSource(SshRemoteConfig config, StreamLauncher customLauncher, bool deferQuery)
    : config_(std::move(config)), launcher_(std::move(customLauncher)), deferQuery_(deferQuery) {
    info_.type = "remote";
    info_.displayPath = config_.remotePath;
    info_.host = config_.host;
    info_.port = config_.port;
    info_.username = config_.username;
    info_.status = "Failed";

    std::wstring err;
    if (deferQuery_) {
        if (!launcher_ && !ValidateRemoteSshConfig(config_, err)) {
            info_.error = err;
            return;
        }
        if (launcher_ && (config_.remotePath.empty() || config_.remotePath[0] != L'/' ||
            std::any_of(config_.remotePath.begin(), config_.remotePath.end(), [](wchar_t ch) { return ch < 32; }))) {
            info_.error = L"远程路径必须是以 '/' 开头的绝对 POSIX 路径，且不能包含控制字符";
            return;
        }
        info_.status = "Pending";
        return;
    }
    if (!QueryRemoteAttributes(err)) {
        info_.status = "Failed";
        info_.error = err.empty() ? L"远程文件不可访问" : err;
    } else {
        info_.status = "Ready";
    }
}

RemoteSshTftpFileSource::RemoteSshTftpFileSource(SshRemoteConfig config, StreamLauncher customLauncher,
                                               TftpSourceInfo prequeriedInfo, std::wstring snapshot, bool deferQuery)
    : config_(std::move(config)), launcher_(std::move(customLauncher)), info_(std::move(prequeriedInfo)),
      snapshot_(std::move(snapshot)), deferQuery_(deferQuery) {
}

RemoteSshTftpFileSource::~RemoteSshTftpFileSource() {
    Close();
}

TftpSourceInfo RemoteSshTftpFileSource::Info() const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return info_;
}

std::shared_ptr<ITftpFileSource> RemoteSshTftpFileSource::CreateSessionSource() const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return std::shared_ptr<ITftpFileSource>(
        new RemoteSshTftpFileSource(config_, launcher_, info_, snapshot_, deferQuery_));
}

bool RemoteSshTftpFileSource::QueryRemoteAttributes(std::wstring& error) {
    info_.notFound = false;
    if (launcher_) {
        if (config_.remotePath.empty() || config_.remotePath[0] != L'/' ||
            std::any_of(config_.remotePath.begin(), config_.remotePath.end(), [](wchar_t ch) { return ch < 32; })) {
            error = L"远程路径必须是以 '/' 开头的绝对 POSIX 路径，且不能包含控制字符";
            return false;
        }
        info_.size = 10485760;
        info_.sizeKnown = true;
        return true;
    }
    if (!ValidateRemoteSshConfig(config_, error)) return false;

    const auto path = L"'" + EscapePosixSingleQuotes(config_.remotePath) + L"'";
    const auto command = L"f=" + path + L"; "
        L"if [ ! -e \"$f\" ] && [ ! -L \"$f\" ]; then exit 42; fi; "
        L"[ \"$(realpath -e -- \"$f\")\" = \"$f\" ] && [ -f \"$f\" ] && [ -r \"$f\" ] || exit 43; " +
        std::wstring(RemoteStat) + L"\"$f\"";
    std::string output;
    DWORD exitCode = 1;
    if (!RunPlinkCommand(config_, command, 512, 8000, output, exitCode, error)) {
        return false;
    }
    if (exitCode == 42) {
        info_.notFound = true;
        error = L"远程文件不存在";
        return false;
    }
    if (exitCode == 43) {
        error = L"远程文件不是可读常规文件或包含符号链接";
        return false;
    }
    if (exitCode != 0) {
        error = L"远程文件不存在、不可读、含链接或 SSH 检查失败";
        return false;
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    uint64_t size = 0;
    const auto result = std::from_chars(output.data(), output.data() + output.size(), size);
    if (result.ec != std::errc{} || result.ptr == output.data() + output.size() || *result.ptr != ' ' ||
        output.find_first_of("\r\n") != std::string::npos) {
        error = L"解析远程文件信息失败";
        return false;
    }
    snapshot_ = MultiByteToWide(reinterpret_cast<const uint8_t*>(output.data()), output.size(), CP_UTF8);
    if (snapshot_.empty()) {
        error = L"解析远程文件信息失败";
        return false;
    }
    info_.size = size;
    info_.sizeKnown = true;
    return true;
}

bool RemoteSshTftpFileSource::Open(std::wstring& error) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    std::lock_guard<std::mutex> lock(bufferMutex_);
    if (procHandle_ != nullptr) {
        return true;
    }
    if (info_.status == "Pending") {
        std::wstring queryErr;
        if (!QueryRemoteAttributes(queryErr)) {
            info_.status = "Failed";
            info_.error = queryErr.empty() ? L"远程文件不可访问" : queryErr;
            error = info_.error;
            return false;
        }
        info_.status = "Ready";
        info_.error.clear();
    } else if (info_.status != "Ready") {
        error = info_.error.empty() ? L"远程文件不可用" : info_.error;
        return false;
    }

    cancelled_ = false;
    streamEnded_ = false;
    streamError_.clear();
    buffer_.clear();
    bufferStartOffset_ = 0;
    streamReadOffset_ = 0;

    if (launcher_) {
        if (!launcher_(config_, outputRead_, procHandle_, error)) {
            return false;
        }
    } else {
        passwordPipe_.Reset();
        if (!config_.password.empty() && !passwordPipe_.Open(config_.password, error)) return false;
        const auto path = L"'" + EscapePosixSingleQuotes(config_.remotePath) + L"'";
        const auto expected = L"'" + EscapePosixSingleQuotes(snapshot_) + L"'";
        const auto command = L"f=" + path + L"; expected=" + expected + L"; "
            L"[ \"$(realpath -e -- \"$f\")\" = \"$f\" ] && [ -f \"$f\" ] || exit 1; "
            L"exec 3<\"$f\" || exit 1; "
            L"[ -f /proc/self/fd/3 ] && [ \"$(" + std::wstring(RemoteStat) +
            L"/proc/self/fd/3)\" = \"$expected\" ] && cat <&3 && "
            L"[ \"$(" + RemoteStat + L"/proc/self/fd/3)\" = \"$expected\" ] && "
            L"[ \"$(" + RemoteStat + L"\"$f\")\" = \"$expected\" ]";
        if (!LaunchPlink(config_, passwordPipe_.Name(), command, false, outputRead_, procHandle_, error)) {
            passwordPipe_.Reset();
            return false;
        }
    }

    try {
        readerThread_ = std::thread(&RemoteSshTftpFileSource::ReaderThread, this);
    } catch (const std::system_error&) {
        TerminateProcess(procHandle_, 1);
        WaitForSingleObject(procHandle_, 1000);
        CloseHandle(std::exchange(procHandle_, nullptr));
        CloseHandle(std::exchange(outputRead_, nullptr));
        passwordPipe_.Reset();
        error = L"无法启动远程文件读取线程";
        return false;
    }
    return true;
}

void RemoteSshTftpFileSource::ReaderThread() {
    std::array<uint8_t, 32768> chunk{};
    std::wstring failure;
    while (!cancelled_) {
        if (!launcher_ && !config_.password.empty()) {
            passwordPipe_.Poll();
            if (passwordPipe_.Failed()) {
                failure = L"远程文件 SSH 认证失败";
                break;
            }
        }
        size_t capacity = 0;
        {
            std::unique_lock<std::mutex> lock(bufferMutex_);
            cvBuffer_.wait(lock, [this] { return cancelled_ || buffer_.size() < MaxBufferCapacity(); });
            if (cancelled_) break;
            capacity = MaxBufferCapacity() - buffer_.size();
        }
        DWORD available = 0;
        if (!PeekNamedPipe(outputRead_, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) {
                failure = L"远程文件输出通道读取失败";
                break;
            }
        }
        if (!available) {
            if (WaitForSingleObject(procHandle_, 10) == WAIT_OBJECT_0) {
                if (PeekNamedPipe(outputRead_, nullptr, 0, nullptr, &available, nullptr) && available) continue;
                DWORD exitCode = 1;
                if (!GetExitCodeProcess(procHandle_, &exitCode) || exitCode != 0)
                    failure = L"远程文件已变化或 SSH 读取失败";
                else if (info_.sizeKnown && streamReadOffset_ != info_.size)
                    failure = L"远程文件在预期长度之前结束";
                break;
            }
            continue;
        }
        DWORD read = 0;
        const auto count = std::min({static_cast<size_t>(available), chunk.size(), capacity});
        if (!ReadFile(outputRead_, chunk.data(), static_cast<DWORD>(count), &read, nullptr) || !read) {
            failure = L"远程文件输出通道读取失败";
            break;
        }
        if (info_.sizeKnown && read > info_.size - streamReadOffset_) {
            failure = L"远程文件已超过发布时的大小";
            break;
        }
        std::lock_guard<std::mutex> lock(bufferMutex_);
        buffer_.insert(buffer_.end(), chunk.data(), chunk.data() + read);
        streamReadOffset_ += read;
        cvBuffer_.notify_all();
    }
    std::lock_guard<std::mutex> lock(bufferMutex_);
    streamEnded_ = true;
    streamError_ = std::move(failure);
    cvBuffer_.notify_all();
}

void RemoteSshTftpFileSource::Close() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        cancelled_ = true;
        cvBuffer_.notify_all();
    }
    if (procHandle_) TerminateProcess(procHandle_, 1);
    if (readerThread_.joinable()) readerThread_.join();
    if (outputRead_) CloseHandle(std::exchange(outputRead_, nullptr));
    if (procHandle_) {
        WaitForSingleObject(procHandle_, 1000);
        CloseHandle(std::exchange(procHandle_, nullptr));
    }
    passwordPipe_.Reset();
    std::lock_guard<std::mutex> lock(bufferMutex_);
    buffer_.clear();
}

bool RemoteSshTftpFileSource::Read(uint64_t offset, void* buffer, size_t length, size_t& bytesRead, std::wstring& error) {
    bytesRead = 0;
    std::unique_lock<std::mutex> lock(bufferMutex_);

    // Discard already ACKed data before offset
    if (offset > bufferStartOffset_) {
        size_t toDrop = static_cast<size_t>(offset - bufferStartOffset_);
        if (toDrop <= buffer_.size()) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + toDrop);
            bufferStartOffset_ = offset;
            cvBuffer_.notify_one(); // signal backpressure reader
        }
    }

    // Wait until requested data is in buffer or EOF or error
    const bool finalRead = info_.sizeKnown && (offset >= info_.size || length >= info_.size - offset);
    const bool ready = cvBuffer_.wait_for(lock, std::chrono::seconds(15), [this, offset, length, finalRead] {
        if (cancelled_ || streamEnded_) return true;
        // Do not acknowledge the last block until plink exits successfully and the
        // remote command has verified the source snapshot after streaming.
        return !finalRead && bufferStartOffset_ + buffer_.size() >= offset + length;
    });

    if (cancelled_) {
        error = L"Transfer cancelled";
        return false;
    }
    if (!streamError_.empty()) {
        error = streamError_;
        return false;
    }
    if (!ready) {
        error = L"Remote stream read timeout";
        return false;
    }

    if (offset < bufferStartOffset_) {
        error = L"Requested offset rolled out of sliding window";
        return false;
    }

    size_t rel = static_cast<size_t>(offset - bufferStartOffset_);
    size_t avail = (buffer_.size() > rel) ? (buffer_.size() - rel) : 0;
    size_t count = std::min(length, avail);

    if (count > 0) {
        std::copy(buffer_.begin() + rel, buffer_.begin() + rel + count, static_cast<uint8_t*>(buffer));
        bytesRead = count;
        return true;
    }

    if (streamEnded_) {
        bytesRead = 0; // EOF
        return true;
    }

    error = L"Unexpected stream state";
    return false;
}

void RemoteSshTftpFileSource::Cancel() {
    Close();
}

size_t RemoteSshTftpFileSource::CurrentBufferSize() const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return buffer_.size();
}

// ============================================================================
// MockStreamTftpFileSource
// ============================================================================

MockStreamTftpFileSource::MockStreamTftpFileSource(uint64_t totalSize, StreamGenerator generator)
    : totalSize_(totalSize), generator_(std::move(generator)) {
    if (!generator_) {
        // Default deterministic generator: byte = (offset & 0xFF)
        generator_ = [](uint64_t offset, uint8_t* dest, size_t len) {
            for (size_t i = 0; i < len; ++i) {
                dest[i] = static_cast<uint8_t>((offset + i) & 0xFF);
            }
        };
    }
}

MockStreamTftpFileSource::~MockStreamTftpFileSource() {
    Close();
}

TftpSourceInfo MockStreamTftpFileSource::Info() const {
    TftpSourceInfo info;
    info.type = "remote";
    info.displayPath = L"mock://virtual-stream.bin";
    info.host = L"virtual-host";
    info.port = 22;
    info.username = L"testuser";
    info.size = totalSize_;
    info.sizeKnown = true;
    info.status = "Ready";
    return info;
}

std::shared_ptr<ITftpFileSource> MockStreamTftpFileSource::CreateSessionSource() const {
    return std::const_pointer_cast<MockStreamTftpFileSource>(shared_from_this());
}

bool MockStreamTftpFileSource::Open(std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = false;
    if (!injectedError_.empty()) {
        error = injectedError_;
        return false;
    }
    return true;
}

void MockStreamTftpFileSource::Close() {
    cancelled_ = true;
}

bool MockStreamTftpFileSource::Read(uint64_t offset, void* buffer, size_t length, size_t& bytesRead, std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!injectedError_.empty()) {
        error = injectedError_;
        return false;
    }
    if (cancelled_) {
        error = L"Cancelled";
        return false;
    }

    if (offset >= totalSize_) {
        bytesRead = 0; // EOF
        return true;
    }

    size_t count = std::min(static_cast<uint64_t>(length), totalSize_ - offset);
    if (generator_) {
        generator_(offset, static_cast<uint8_t*>(buffer), count);
    }
    bytesRead = count;
    return true;
}

void MockStreamTftpFileSource::Cancel() {
    Close();
}

namespace {
void SortTftpDirectoryEntries(std::vector<TftpDirectoryEntry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const TftpDirectoryEntry& a, const TftpDirectoryEntry& b) {
        std::string al = a.name, bl = b.name;
        for (char& ch : al) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (char& ch : bl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (al != bl) return al < bl;
        return a.name < b.name;
    });
}
} // namespace

bool ValidateLocalTftpDirectory(const std::wstring& dirPath, std::wstring& normalizedPath, std::wstring& error) {
    normalizedPath.clear();
    const std::wstring trimmed = TrimWhitespace(dirPath);
    if (trimmed.empty()) {
        error = L"请选择或输入本地 TFTP 目录";
        return false;
    }
    std::filesystem::path dir(trimmed);
    dir = dir.lexically_normal();
    std::wstring native = dir.native();
    const size_t rootLen = dir.root_path().native().size();
    while (native.size() > rootLen && (native.back() == L'\\' || native.back() == L'/')) {
        native.pop_back();
    }
    dir = std::filesystem::path(native);

    std::vector<std::unique_ptr<SourceHandle>> directories;
    bool notFound = false;
    if (!OpenLocalVerifiedDirectoryChain(dir, directories, notFound)) {
        error = notFound ? L"本地目录不存在" : L"本地路径不是有效目录，或包含符号链接/重解析点";
        return false;
    }
    normalizedPath = dir.native();
    return true;
}

bool ListLocalTftpDirectory(const std::wstring& dirPath, std::vector<TftpDirectoryEntry>& entries, std::wstring& error) {
    entries.clear();
    std::wstring normalized;
    if (!ValidateLocalTftpDirectory(dirPath, normalized, error)) {
        return false;
    }
    const auto pattern = (std::filesystem::path(normalized) / L"*").native();
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES) {
            return true;
        }
        error = L"读取本地目录内容失败";
        return false;
    }
    struct FindGuard {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~FindGuard() {
            if (handle != INVALID_HANDLE_VALUE) FindClose(handle);
        }
    } guard{find};

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_OFFLINE)) {
            continue;
        }
        const std::string utf8Name = WideToMultiByte(fd.cFileName, CP_UTF8);
        if (utf8Name.empty()) continue;
        TftpDirectoryEntry entry;
        entry.name = utf8Name;
        entry.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        entry.sizeKnown = true;
        entries.push_back(std::move(entry));
    } while (FindNextFileW(find, &fd));

    const DWORD err = GetLastError();
    if (err != ERROR_NO_MORE_FILES) {
        entries.clear();
        error = L"枚举本地目录文件时发生错误";
        return false;
    }

    SortTftpDirectoryEntries(entries);
    return true;
}

bool ValidateRemoteTftpDirectoryConfig(SshRemoteConfig& config, std::wstring& error) {
    std::wstring path = TrimWhitespace(config.remotePath);
    while (path.size() > 1 && path.back() == L'/') {
        path.pop_back();
    }
    config.remotePath = path;
    if (!ValidateRemoteSshConfig(config, error)) {
        return false;
    }
    return true;
}

bool ListRemoteTftpDirectory(const SshRemoteConfig& config, std::vector<TftpDirectoryEntry>& entries, std::wstring& error) {
    entries.clear();
    SshRemoteConfig cfg = config;
    if (!ValidateRemoteTftpDirectoryConfig(cfg, error)) {
        return false;
    }
    const auto dir = L"'" + EscapePosixSingleQuotes(cfg.remotePath) + L"'";
    const auto command = L"d=" + dir + L"; "
        L"if [ ! -e \"$d\" ] && [ ! -L \"$d\" ]; then exit 42; fi; "
        L"[ \"$(realpath -e -- \"$d\")\" = \"$d\" ] && [ -d \"$d\" ] && [ -r \"$d\" ] && [ -x \"$d\" ] || exit 43; "
        L"for p in \"$d\"/*; do "
        L"[ -L \"$p\" ] && continue; "
        L"[ -f \"$p\" ] && [ -r \"$p\" ] || continue; "
        L"sz=$(LC_ALL=C stat -c '%s' -- \"$p\" 2>/dev/null || wc -c < \"$p\" 2>/dev/null || printf '?'); "
        L"printf '%s\t%s\n' \"$sz\" \"${p##*/}\"; "
        L"done";
    std::string output;
    DWORD exitCode = 1;
    if (!RunPlinkCommand(cfg, command, 512 * 1024, 8000, output, exitCode, error)) {
        return false;
    }
    if (exitCode == 42) {
        error = L"远程目录不存在";
        return false;
    }
    if (exitCode == 43) {
        error = L"远程路径不是有效可读目录，或包含符号链接";
        return false;
    }
    if (exitCode != 0) {
        error = L"读取远程目录失败，请确认 SSH 会话认证与权限";
        return false;
    }

    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string sizeStr = line.substr(0, tab);
        std::string name = line.substr(tab + 1);
        if (name.empty() || name == "." || name == ".." ||
            name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name.find('\0') != std::string::npos) {
            continue;
        }
        TftpDirectoryEntry entry;
        entry.name = std::move(name);
        size_t firstNonSpace = sizeStr.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos) {
            uint64_t parsedSize = 0;
            const auto res = std::from_chars(sizeStr.data() + firstNonSpace, sizeStr.data() + sizeStr.size(), parsedSize);
            if (res.ec == std::errc{} && res.ptr == sizeStr.data() + sizeStr.size()) {
                entry.size = parsedSize;
                entry.sizeKnown = true;
            }
        }
        entries.push_back(std::move(entry));
    }

    SortTftpDirectoryEntries(entries);
    return true;
}

} // namespace serialctl
