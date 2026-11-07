#include "TftpFileSource.h"
#include "ChildProcess.h"
#include "PasswordPipe.h"
#include "FilePaths.h"
#include <algorithm>
#include <array>

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

// ============================================================================
// LocalTftpFileSource
// ============================================================================

LocalTftpFileSource::LocalTftpFileSource(std::wstring path)
    : path_(std::move(path)) {
    info_.type = "local";
    info_.displayPath = path_;
    info_.status = "Failed";

    DWORD attr = GetFileAttributesW(path_.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        info_.error = L"文件不存在或无法访问";
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        info_.error = L"路径是目录，不是常规文件";
        return;
    }

    HANDLE h = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        info_.error = L"无法读取文件（可能被独占或权限不足）";
        return;
    }

    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz)) {
        size_ = static_cast<uint64_t>(sz.QuadPart);
        info_.size = size_;
        info_.sizeKnown = true;
    }
    GetFileTime(h, nullptr, nullptr, &lastWriteTime_);
    CloseHandle(h);

    info_.status = "Ready";
}

LocalTftpFileSource::LocalTftpFileSource(std::wstring path, FILETIME expectedWriteTime, uint64_t expectedSize, TftpSourceInfo info)
    : path_(std::move(path)), lastWriteTime_(expectedWriteTime), size_(expectedSize), info_(std::move(info)) {
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
    return std::make_shared<LocalTftpFileSource>(path_, lastWriteTime_, size_, info_);
}

bool LocalTftpFileSource::Open(std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        return true;
    }

    file_ = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        error = L"无法打开本地文件: 错误代码 " + std::to_wstring(err);
        return false;
    }

    LARGE_INTEGER curSz{};
    FILETIME curTime{};
    if ((GetFileTime(file_, nullptr, nullptr, &curTime) && CompareFileTime(&curTime, &lastWriteTime_) != 0) ||
        (GetFileSizeEx(file_, &curSz) && static_cast<uint64_t>(curSz.QuadPart) != size_)) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        error = L"Source file modified during transfer";
        return false;
    }
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

    LARGE_INTEGER curSz{};
    FILETIME curTime{};
    if ((GetFileTime(file_, nullptr, nullptr, &curTime) && CompareFileTime(&curTime, &lastWriteTime_) != 0) ||
        (GetFileSizeEx(file_, &curSz) && static_cast<uint64_t>(curSz.QuadPart) != size_)) {
        error = L"Source file modified during transfer";
        return false;
    }

    if (offset >= size_) {
        bytesRead = 0;
        return true;
    }

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file_, li, nullptr, FILE_BEGIN)) {
        error = L"Seek failed";
        return false;
    }

    size_t toRead = std::min(static_cast<uint64_t>(length), size_ - offset);
    DWORD readBytes = 0;
    if (!ReadFile(file_, buffer, static_cast<DWORD>(toRead), &readBytes, nullptr)) {
        error = L"Read failed";
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

RemoteSshTftpFileSource::RemoteSshTftpFileSource(SshRemoteConfig config, StreamLauncher customLauncher, TftpSourceInfo prequeriedInfo)
    : config_(std::move(config)), launcher_(std::move(customLauncher)), info_(std::move(prequeriedInfo)) {
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
    return std::make_shared<RemoteSshTftpFileSource>(config_, launcher_, info_);
}

bool RemoteSshTftpFileSource::QueryRemoteAttributes(std::wstring& error) {
    // If a custom launcher was provided and no production plink is needed:
    if (launcher_) {
        info_.size = 10485760; // 10 MiB default if launcher mocked
        info_.sizeKnown = true;
        return true;
    }

    if (config_.host.empty() || config_.username.empty()) {
        error = L"SSH 主机或用户名为空";
        return false;
    }

    // Production: execute remote stat via plink
    std::wstring plinkPath;
    std::vector<wchar_t> modPath(MAX_PATH);
    if (GetModuleFileNameW(nullptr, modPath.data(), static_cast<DWORD>(modPath.size()))) {
        std::wstring dir = modPath.data();
        auto pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) dir = dir.substr(0, pos + 1);
        plinkPath = dir + L"plink.exe";
    }
    if (plinkPath.empty() || GetFileAttributesW(plinkPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"未找到配套 plink.exe";
        return false;
    }

    PasswordPipe pwPipe;
    if (!config_.password.empty() && !pwPipe.Open(config_.password, error)) {
        return false;
    }

    std::wstring cmd = L"\"" + plinkPath + L"\" -batch -P " + std::to_wstring(config_.port) +
                       L" -l \"" + config_.username + L"\"";
    if (!config_.password.empty()) {
        cmd += L" -pwfile \"" + pwPipe.Name() + L"\"";
    }
    if (!config_.hostKey.empty()) {
        cmd += L" -hostkey \"" + config_.hostKey + L"\"";
    }
    cmd += L" \"" + config_.host + L"\" \"stat -L -c '%s %Y' '" + EscapePosixSingleQuotes(config_.remotePath) + L"' 2>/dev/null\"";

    HANDLE outRead = nullptr, outWrite = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&outRead, &outWrite, &sa, 4096)) {
        error = L"无法创建诊断管道";
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outWrite);

    if (!ok) {
        CloseHandle(outRead);
        error = L"启动 plink 检查远程文件失败";
        return false;
    }

    std::string output;
    char buf[256];
    const ULONGLONG deadline = GetTickCount64() + 8000;
    while (GetTickCount64() < deadline) {
        if (!config_.password.empty()) {
            pwPipe.Poll();
            if (pwPipe.Failed()) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }
        DWORD avail = 0;
        if (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD read = 0;
            if (ReadFile(outRead, buf, std::min<DWORD>(avail, sizeof(buf) - 1), &read, nullptr) && read > 0) {
                output.append(buf, read);
            }
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            DWORD availRem = 0;
            while (PeekNamedPipe(outRead, nullptr, 0, nullptr, &availRem, nullptr) && availRem > 0) {
                DWORD read = 0;
                if (!ReadFile(outRead, buf, std::min<DWORD>(availRem, sizeof(buf) - 1), &read, nullptr) || read == 0) break;
                output.append(buf, read);
            }
            break;
        }
    }
    CloseHandle(outRead);
    if (WaitForSingleObject(pi.hProcess, 100) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 500);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0 || output.empty()) {
        error = L"远程文件不存在或无权访问";
        return false;
    }

    uint64_t sz = 0, mtime = 0;
    if (sscanf_s(output.c_str(), "%llu %llu", &sz, &mtime) >= 1) {
        info_.size = sz;
        info_.sizeKnown = true;
        info_.lastModified = mtime;
        return true;
    }

    error = L"解析远程文件信息失败";
    return false;
}

bool RemoteSshTftpFileSource::Open(std::wstring& error) {
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
        std::wstring plinkPath;
        std::vector<wchar_t> modPath(MAX_PATH);
        if (GetModuleFileNameW(nullptr, modPath.data(), static_cast<DWORD>(modPath.size()))) {
            std::wstring dir = modPath.data();
            auto pos = dir.find_last_of(L"\\/");
            if (pos != std::wstring::npos) dir = dir.substr(0, pos + 1);
            plinkPath = dir + L"plink.exe";
        }
        passwordPipe_.Reset();
        if (!config_.password.empty() && !passwordPipe_.Open(config_.password, error)) {
            return false;
        }

        std::wstring cmd = L"\"" + plinkPath + L"\" -batch -P " + std::to_wstring(config_.port) +
                           L" -l \"" + config_.username + L"\"";
        if (!config_.password.empty()) cmd += L" -pwfile \"" + passwordPipe_.Name() + L"\"";
        if (!config_.hostKey.empty()) cmd += L" -hostkey \"" + config_.hostKey + L"\"";
        cmd += L" \"" + config_.host + L"\" \"cat -- '" + EscapePosixSingleQuotes(config_.remotePath) + L"'\"";

        HANDLE outRead = nullptr, outWrite = nullptr;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        if (!CreatePipe(&outRead, &outWrite, &sa, 65536)) {
            passwordPipe_.Reset();
            error = L"无法创建传输管道";
            return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = outWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
        cmdLine.push_back(L'\0');

        BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(outWrite);

        if (!ok) {
            CloseHandle(outRead);
            passwordPipe_.Reset();
            error = L"启动流式 plink 失败";
            return false;
        }
        CloseHandle(pi.hThread);
        procHandle_ = pi.hProcess;
        outputRead_ = outRead;
    }

    readerThread_ = std::thread(&RemoteSshTftpFileSource::ReaderThread, this);
    return true;
}

void RemoteSshTftpFileSource::ReaderThread() {
    std::array<uint8_t, 32768> chunk;
    while (!cancelled_) {
        if (!launcher_ && !config_.password.empty() && !passwordPipe_.Delivered() && !passwordPipe_.Failed()) {
            passwordPipe_.Poll();
            DWORD avail = 0;
            if (!PeekNamedPipe(outputRead_, nullptr, 0, nullptr, &avail, nullptr)) {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                streamEnded_ = true;
                if (info_.sizeKnown && streamReadOffset_ < info_.size) {
                    streamError_ = L"Remote stream ended prematurely before complete file was sent";
                }
                cvBuffer_.notify_all();
                break;
            }
            if (avail == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }
        // Enforce backpressure: wait if buffer exceeds capacity
        {
            std::unique_lock<std::mutex> lock(bufferMutex_);
            cvBuffer_.wait(lock, [this] {
                return cancelled_ || buffer_.size() < MaxBufferCapacity();
            });
            if (cancelled_) break;
        }

        DWORD read = 0;
        BOOL ok = ReadFile(outputRead_, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr);
        if (!ok || read == 0) {
            // EOF or pipe broken
            std::lock_guard<std::mutex> lock(bufferMutex_);
            streamEnded_ = true;
            if (info_.sizeKnown && streamReadOffset_ < info_.size) {
                streamError_ = L"Remote stream ended prematurely before complete file was sent";
            }
            cvBuffer_.notify_all();
            break;
        }

        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            buffer_.insert(buffer_.end(), chunk.data(), chunk.data() + read);
            streamReadOffset_ += read;
            cvBuffer_.notify_all();
        }
    }
}

void RemoteSshTftpFileSource::Close() {
    cancelled_ = true;
    cvBuffer_.notify_all();

    if (outputRead_ != nullptr) {
        CloseHandle(outputRead_);
        outputRead_ = nullptr;
    }
    if (procHandle_ != nullptr) {
        TerminateProcess(procHandle_, 0);
        CloseHandle(procHandle_);
        procHandle_ = nullptr;
    }
    if (readerThread_.joinable()) {
        readerThread_.join();
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
    bool ready = cvBuffer_.wait_for(lock, std::chrono::seconds(15), [this, offset, length] {
        if (cancelled_ || streamEnded_ || !streamError_.empty()) return true;
        return (bufferStartOffset_ + buffer_.size()) >= (offset + length);
    });

    if (cancelled_) {
        error = L"Transfer cancelled";
        return false;
    }
    if (!streamError_.empty()) {
        error = streamError_;
        return false;
    }
    if (!ready && bufferStartOffset_ + buffer_.size() < offset) {
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
