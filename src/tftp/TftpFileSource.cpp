#include "TftpFileSource.h"
#include "ChildProcess.h"
#include "PasswordPipe.h"
#include "PuttyHostKey.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <array>
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

HANDLE OpenLocalSource(const std::wstring& path) {
    namespace fs = std::filesystem;
    const fs::path file(path);
    if (!file.is_absolute() || file.filename().empty() || path.find(L'\0') != std::wstring::npos ||
        path.rfind(L"\\\\.\\", 0) == 0 ||
        file.relative_path().native().find(L':') != std::wstring::npos)
        return INVALID_HANDLE_VALUE;
    for (const auto& part : file.relative_path()) {
        const auto& name = part.native();
        if (name.empty() || name.back() == L'.' || name.back() == L' ') return INVALID_HANDLE_VALUE;
    }
    // Open each ancestor without following a reparse point, and keep it open
    // against replacement until the final handle has been acquired.
    auto parent = file.root_path();
    std::vector<std::unique_ptr<SourceHandle>> directories;
    for (const auto& part : file.parent_path().relative_path()) {
        parent /= part;
        auto handle = std::make_unique<SourceHandle>();
        handle->value = CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION directory{};
        if (handle->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle->value, &directory) ||
            !(directory.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (directory.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return INVALID_HANDLE_VALUE;
        directories.push_back(std::move(handle));
    }
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
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
    SourceHandle file;
    file.value = OpenLocalSource(path_);
    if (file.value == INVALID_HANDLE_VALUE || !ReadSnapshot(file.value, snapshot_)) {
        info_.error = L"无法读取常规文件，或路径包含重解析点";
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

RemoteSshTftpFileSource::RemoteSshTftpFileSource(SshRemoteConfig config, StreamLauncher customLauncher)
    : config_(std::move(config)), launcher_(std::move(customLauncher)) {
    info_.type = "remote";
    info_.displayPath = config_.remotePath;
    info_.host = config_.host;
    info_.port = config_.port;
    info_.username = config_.username;
    info_.status = "Failed";

    std::wstring err;
    if (!QueryRemoteAttributes(err)) {
        info_.status = "Failed";
        info_.error = err.empty() ? L"远程文件不可访问" : err;
    } else {
        info_.status = "Ready";
    }
}

RemoteSshTftpFileSource::RemoteSshTftpFileSource(SshRemoteConfig config, StreamLauncher customLauncher,
                                               TftpSourceInfo prequeriedInfo, std::wstring snapshot)
    : config_(std::move(config)), launcher_(std::move(customLauncher)), info_(std::move(prequeriedInfo)),
      snapshot_(std::move(snapshot)) {
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
    return std::shared_ptr<ITftpFileSource>(new RemoteSshTftpFileSource(config_, launcher_, info_, snapshot_));
}

bool RemoteSshTftpFileSource::QueryRemoteAttributes(std::wstring& error) {
    if (config_.remotePath.empty() || config_.remotePath[0] != L'/' ||
        std::any_of(config_.remotePath.begin(), config_.remotePath.end(), [](wchar_t ch) { return ch < 32; })) {
        error = L"远程路径必须是以 '/' 开头的绝对 POSIX 路径，且不能包含控制字符";
        return false;
    }
    if (launcher_) {
        info_.size = 10485760;
        info_.sizeKnown = true;
        return true;
    }
    if (config_.host.empty() || config_.host.front() == L'-' || config_.username.empty()) {
        error = L"SSH 主机或用户名无效";
        return false;
    }
    if (config_.hostKey.empty()) config_.hostKey = TrustedPuttyHostKey(config_.host, config_.port);
    if (config_.hostKey.empty()) {
        error = L"远程发布需要先在 SerialCtl 中核验该 SSH 主机的身份";
        return false;
    }

    PasswordPipe password;
    if (!config_.password.empty() && !password.Open(config_.password, error)) return false;
    const auto path = L"'" + EscapePosixSingleQuotes(config_.remotePath) + L"'";
    const auto command = L"f=" + path + L"; [ \"$(realpath -e -- \"$f\")\" = \"$f\" ] && "
        L"[ -f \"$f\" ] && [ -r \"$f\" ] && " + RemoteStat + L"\"$f\"";
    SourceHandle pipe, child;
    if (!LaunchPlink(config_, password.Name(), command, true, pipe.value, child.value, error)) return false;

    std::string output;
    std::array<char, 512> buffer{};
    const auto deadline = GetTickCount64() + 8000;
    bool exited = false;
    while (GetTickCount64() < deadline) {
        if (!config_.password.empty()) {
            password.Poll();
            if (password.Failed()) break;
        }
        DWORD available = 0, read = 0;
        if (PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            if (!ReadFile(pipe.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())),
                          &read, nullptr) || !read || output.size() + read > buffer.size())
                break;
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
    }
    DWORD exitCode = 1;
    if (!exited || !GetExitCodeProcess(child.value, &exitCode) || exitCode != 0) {
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
    if (info_.status != "Ready") {
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

} // namespace serialctl
