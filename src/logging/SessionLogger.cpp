#include "SessionLogger.h"
#include "Win32Helpers.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <shlobj.h>

namespace serialctl
{

SessionLogger::~SessionLogger()
{
    StopContinuousSave();
    Stop();
}

bool SessionLogger::Start(const std::wstring &mode, std::wstring &error, bool csv)
{
    StopContinuousSave();
    Stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        path_.clear();
        parts_.clear();
        accepted_ = completed_ = written_ = 0;
        queued_ = highWaterQueued_ = 0;
        overflowEvents_ = 0;
        failure_.clear();
        wake_.notify_all();
    }
    std::array<wchar_t, MAX_PATH> appData{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT,
                                appData.data())))
    {
        error = L"无法找到本地应用数据目录";
        return false;
    }
    std::wstring directory = std::wstring(appData.data()) + L"\\SerialCtl\\logs";
    if (SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr) != ERROR_SUCCESS &&
        GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error = L"无法创建日志目录：" + Win32ErrorMessage();
        return false;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t baseName[64]{};
    swprintf_s(baseName, L"\\%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute, now.wSecond);
    const std::wstring extension = csv ? L".csv" : L".log";
    std::wstring createdPath;
    for (unsigned int suffix = 1; suffix <= 100 && file_ == INVALID_HANDLE_VALUE; ++suffix)
    {
        createdPath = directory + baseName +
                (suffix == 1 ? extension : L"-" + std::to_wstring(suffix) + extension);
        file_ = CreateFileW(createdPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
            break;
    }
    if (file_ == INVALID_HANDLE_VALUE)
    {
        error = L"无法创建日志：" + Win32ErrorMessage();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = std::move(createdPath);
        parts_.push_back({path_, 0});
        stopping_ = false;
        writerAlive_ = true;
    }
    try {
        worker_ = std::thread(&SessionLogger::Run, this);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            writerAlive_ = false;
            failure_ = error = L"无法启动日志写入线程";
            wake_.notify_all();
        }
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        return false;
    }
    if (!csv)
        WriteStatus(L"SerialCtl 会话开始，模式：" + mode);
    return true;
}

bool SessionLogger::Resume(const std::wstring &mode, std::wstring &error) {
    Stop();
    std::wstring lastPath;
    std::uint64_t expected = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path_.empty() || parts_.empty() || !failure_.empty()) {
            error = failure_.empty() ? L"没有可恢复的会话日志" : failure_;
            return false;
        }
        lastPath = parts_.back().path;
        expected = parts_.back().length;
    }
    HANDLE file = CreateFileW(lastPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"无法重新打开历史日志：" + Win32ErrorMessage(); return false; }
    LARGE_INTEGER actual{}, end{};
    end.QuadPart = 0;
    if (!GetFileSizeEx(file, &actual) || actual.QuadPart < 0 ||
        static_cast<std::uint64_t>(actual.QuadPart) != expected ||
        !SetFilePointerEx(file, end, nullptr, FILE_END)) {
        CloseHandle(file);
        error = L"历史日志文件已发生外部变化，拒绝重连追加";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        file_ = file;
        written_ = expected;
        stopping_ = false;
        writerAlive_ = true;
    }
    try { worker_ = std::thread(&SessionLogger::Run, this); }
    catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true; writerAlive_ = false; file_ = INVALID_HANDLE_VALUE;
        }
        CloseHandle(file);
        error = L"无法启动历史日志续写线程";
        return false;
    }
    WriteStatus(L"SerialCtl 会话重新连接，模式：" + mode);
    return true;
}

void SessionLogger::WriteRaw(const Bytes &data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_ && !data.empty())
    {
        WriteBytes(data.data(), static_cast<DWORD>(data.size()));
    }
}

void SessionLogger::WriteText(const std::wstring &text)
{
    if (!text.empty()) WriteBatch({{false, text}});
}

void SessionLogger::WriteStatus(const std::wstring &text)
{
    WriteBatch({{true, text}});
}

void SessionLogger::WriteBatch(const std::vector<std::pair<bool, std::wstring>>& entries)
{
    if (entries.empty()) return;
    std::lock_guard<std::mutex> encodeLock(encodeMutex_);
    std::wstring combined;
    size_t reserve = 0;
    for (const auto& entry : entries) reserve += entry.second.size() + (entry.first ? 32 : 0);
    combined.reserve(reserve);
    for (const auto& entry : entries) {
        if (!entry.first) {
            combined += entry.second;
            continue;
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t prefix[32]{};
        swprintf_s(prefix, L"\r\n[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond,
                   now.wMilliseconds);
        combined += prefix;
        combined += entry.second;
        combined += L"\r\n";
    }
    const std::string utf8 = WideToMultiByte(combined, CP_UTF8);
    std::lock_guard<std::mutex> lock(mutex_);
    WriteBytes(utf8.data(), static_cast<DWORD>(utf8.size()));
}

SessionLogger::Statistics SessionLogger::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {queued_, highWaterQueued_, accepted_, completed_, overflowEvents_};
}

namespace {
struct ExportFile {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit ExportFile(HANDLE handle) : value(handle) {}
    ~ExportFile() { Close(); }
    ExportFile(const ExportFile &) = delete;
    ExportFile &operator=(const ExportFile &) = delete;
    void Close() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
    bool Valid() const { return value != INVALID_HANDLE_VALUE; }
};
bool ExportCancelled(const std::atomic<bool> *cancel) {
    return cancel && cancel->load();
}
bool CancelExport(std::wstring &error) {
    error = L"日志导出已取消；原目标文件未替换";
    return false;
}
bool FullExportPath(const std::wstring &path, std::wstring &result, std::wstring &error) {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= buffer.size()) {
        error = L"无效的日志保存路径";
        return false;
    }
    result.assign(buffer.data(), length);
    return true;
}
bool ExportParent(const std::wstring &full, std::wstring &canonical, std::wstring &error) {
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        error = L"无效的日志保存目录";
        return false;
    }
    const auto parent = full.substr(0, slash == 2 && full[1] == L':' ? 3 : slash);
    ExportFile folder(CreateFileW(parent.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!folder.Valid()) {
        error = L"无法访问日志保存目录：" + Win32ErrorMessage();
        return false;
    }
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFinalPathNameByHandleW(folder.value, buffer.data(),
        static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= buffer.size()) {
        error = L"无法确认日志保存目录：" + Win32ErrorMessage();
        return false;
    }
    canonical.assign(buffer.data(), length);
    return true;
}
bool IsLogFamily(const std::wstring &name, const std::wstring &root) {
    if (_wcsicmp(name.c_str(), root.c_str()) == 0) return true;
    const auto dot = root.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const auto prefix = root.substr(0, dot) + L".part", extension = root.substr(dot);
    if (name.size() <= prefix.size() + extension.size() ||
        _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0 ||
        _wcsicmp(name.c_str() + name.size() - extension.size(), extension.c_str()) != 0) return false;
    return std::all_of(name.begin() + prefix.size(), name.end() - extension.size(),
                       [](wchar_t digit) { return digit >= L'0' && digit <= L'9'; });
}
} // namespace

#ifdef SERIALCTL_LOGGER_TESTING
std::atomic<std::int64_t> SessionLogger::testCopyFailureAfter_{-1};
std::atomic<bool> SessionLogger::testFailFlushCopy_{false};
std::atomic<bool> SessionLogger::testFailFlushStop_{false};
#endif

SessionLogger::ExportPoint SessionLogger::CaptureExportPoint() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {generation_, accepted_, path_};
}

bool SessionLogger::CaptureSnapshot(ExportPoint point, const std::atomic<bool> *cancel,
                                    Snapshot &snapshot, std::wstring &error) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (point.generation != generation_ || point.bytes > accepted_ || path_.empty() || point.root != path_) {
        error = L"原日志已切换或尚未开始，未导出";
        return false;
    }
    // A fixed accepted-byte cutoff cannot be postponed by continuous output.
    // Waiting releases the producer lock; no export I/O runs under that lock.
    while (point.generation == generation_ && failure_.empty() && completed_ < point.bytes &&
           writerAlive_ && !ExportCancelled(cancel))
        wake_.wait_for(lock, std::chrono::milliseconds(50));
    if (ExportCancelled(cancel)) return CancelExport(error);
    if (point.generation != generation_) {
        error = L"原日志已切换，未混入新会话数据";
        return false;
    }
    if (!failure_.empty()) {
        error = failure_;
        return false;
    }
    if (completed_ < point.bytes) {
        error = L"日志写入尚未达到快照截止点，未导出";
        return false;
    }
    snapshot.root = path_;
    snapshot.length = point.bytes;
    auto remaining = point.bytes;
    for (const auto &part : parts_) {
        if (!remaining) break;
        const auto length = std::min(remaining, part.length);
        if (length) snapshot.parts.push_back({part.path, length});
        remaining -= length;
    }
    if (remaining) {
        error = L"日志分卷快照不完整，未导出";
        return false;
    }
    return true;
}

bool SessionLogger::SaveCopy(const std::wstring &destination, std::wstring &error) {
    return SaveCopy(destination, CaptureExportPoint(), error, nullptr);
}

bool SessionLogger::SaveCopy(const std::wstring &destination, ExportPoint point, std::wstring &error,
                             const std::atomic<bool> *cancel, const ExportProgress &progress) {
    Snapshot snapshot;
    if (!CaptureSnapshot(point, cancel, snapshot, error)) return false;
    return CopySnapshot(snapshot, destination, error, cancel, progress);
}

bool SessionLogger::CopySnapshot(const Snapshot &snapshot, const std::wstring &destination,
                                 std::wstring &error, const std::atomic<bool> *cancel,
                                 const ExportProgress &progress) {
    if (ExportCancelled(cancel)) return CancelExport(error);
    std::wstring fullDestination, fullSource, destinationParent, sourceParent;
    if (!FullExportPath(destination, fullDestination, error) ||
        !FullExportPath(snapshot.root, fullSource, error) ||
        !ExportParent(fullDestination, destinationParent, error) ||
        !ExportParent(fullSource, sourceParent, error)) return false;
    const auto destinationName = fullDestination.substr(fullDestination.find_last_of(L"\\/") + 1);
    const auto sourceName = fullSource.substr(fullSource.find_last_of(L"\\/") + 1);
    if (_wcsicmp(destinationParent.c_str(), sourceParent.c_str()) == 0 &&
        IsLogFamily(destinationName, sourceName)) {
        error = L"不能覆盖日志源文件或其分卷";
        return false;
    }
    static std::atomic<unsigned long> nextTemporary{0};
    const auto temporary = fullDestination + L".serialctl-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++nextTemporary) + L".tmp";
    bool temporaryCreated = false, success = false;
    auto copy = [&]() -> bool {
        ExportFile output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!output.Valid()) {
            error = L"无法创建日志副本：" + Win32ErrorMessage();
            return false;
        }
        temporaryCreated = true;
        std::uint64_t copied = 0;
        if (progress) progress(0, snapshot.length);
        std::array<char, 65536> buffer{};
        for (const auto &part : snapshot.parts) {
            if (ExportCancelled(cancel)) return CancelExport(error);
            ExportFile input(CreateFileW(part.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!input.Valid()) {
                error = L"无法读取日志分卷：" + Win32ErrorMessage();
                return false;
            }
            auto remaining = part.length;
            while (remaining) {
                if (ExportCancelled(cancel)) return CancelExport(error);
                DWORD count = 0;
                const auto wanted = static_cast<DWORD>(std::min<std::uint64_t>(remaining, buffer.size()));
                if (!ReadFile(input.value, buffer.data(), wanted, &count, nullptr)) {
                    const auto code = GetLastError();
                    if (ExportCancelled(cancel)) return CancelExport(error);
                    error = L"读取日志分卷失败：" + Win32ErrorMessage(code);
                    return false;
                }
                if (!count) {
                    error = L"日志源文件提前结束，未保存不完整快照";
                    return false;
                }
                DWORD offset = 0;
                while (offset < count) {
                    if (ExportCancelled(cancel)) return CancelExport(error);
#ifdef SERIALCTL_LOGGER_TESTING
                    const auto failAfter = testCopyFailureAfter_.load();
                    if (failAfter >= 0 && copied >= static_cast<std::uint64_t>(failAfter)) {
                        error = L"写入日志副本失败：" + Win32ErrorMessage(ERROR_DISK_FULL);
                        return false;
                    }
#endif
                    DWORD done = 0;
                    const bool wrote = WriteFile(output.value, buffer.data() + offset, count - offset, &done, nullptr) != FALSE;
                    if (!wrote || !done) {
                        const auto code = wrote ? ERROR_WRITE_FAULT : GetLastError();
                        if (ExportCancelled(cancel)) return CancelExport(error);
                        error = L"写入日志副本失败：" + Win32ErrorMessage(code);
                        return false;
                    }
                    offset += done;
                    copied += done;
                    if (progress) progress(copied, snapshot.length);
                }
                remaining -= count;
            }
        }
        if (ExportCancelled(cancel)) return CancelExport(error);
        bool flushOk = FlushFileBuffers(output.value) != FALSE;
#ifdef SERIALCTL_LOGGER_TESTING
        if (testFailFlushCopy_.load()) flushOk = false;
#endif
        if (!flushOk) {
            const auto code = GetLastError();
            if (ExportCancelled(cancel)) return CancelExport(error);
            error = L"日志副本刷新失败：" + Win32ErrorMessage(code);
            return false;
        }
        output.Close();
        if (ExportCancelled(cancel)) return CancelExport(error);
        // Only this final commit replaces the existing target. A successful
        // commit wins a simultaneous cancellation; never report it as canceled.
        if (!MoveFileExW(temporary.c_str(), fullDestination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = L"无法替换日志目标文件：" + Win32ErrorMessage();
            return false;
        }
        return true;
    };
    try { success = copy(); }
    catch (...) { error = L"日志导出失败，原目标文件未替换"; }
    if (!success && temporaryCreated && !DeleteFileW(temporary.c_str())) {
        const auto code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
            error += L"；临时副本未清理：" + temporary;
    }
    return success;
}


bool SessionLogger::CopyCompletedRange(ExportPoint point, std::uint64_t from,
                                       HANDLE output, std::wstring& error) {
    Snapshot snapshot;
    if (from > point.bytes || !CaptureSnapshot(point, nullptr, snapshot, error)) return false;
    std::array<char, 65536> buffer{};
    auto skip = from;
    for (const auto& part : snapshot.parts) {
        if (skip >= part.length) { skip -= part.length; continue; }
        HANDLE input = CreateFileW(part.path.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (input == INVALID_HANDLE_VALUE) {
            error = L"持续保存无法读取日志分卷：" + Win32ErrorMessage(); return false;
        }
        LARGE_INTEGER distance{}; distance.QuadPart = static_cast<LONGLONG>(skip);
        if (!SetFilePointerEx(input, distance, nullptr, FILE_BEGIN)) {
            error = L"持续保存定位源日志失败：" + Win32ErrorMessage(); CloseHandle(input); return false;
        }
        auto remaining = part.length - skip;
        skip = 0;
        while (remaining) {
            DWORD count = 0;
            const auto required = static_cast<DWORD>(std::min<std::uint64_t>(remaining, buffer.size()));
            if (!ReadFile(input, buffer.data(), required, &count, nullptr) || !count) {
                error = L"持续保存读取源日志失败：" + Win32ErrorMessage(); CloseHandle(input); return false;
            }
            DWORD pos = 0;
            while (pos < count) {
                DWORD written = 0;
                if (!WriteFile(output, buffer.data() + pos, count - pos, &written, nullptr) || !written) {
                    error = L"持续保存写入目标失败：" + Win32ErrorMessage(); CloseHandle(input); return false;
                }
                pos += written;
            }
            remaining -= count;
        }
        CloseHandle(input);
    }
    return true;
}

std::wstring SessionLogger::ContinuousSaveError() const {
    std::lock_guard<std::mutex> lock(continuousMutex_);
    return continuousError_;
}

bool SessionLogger::StartContinuousSave(const std::wstring &destination, std::wstring &error) {
    if (continuousActive_.load()) { error = L"已经在持续保存日志"; return false; }
    if (continuousWorker_.joinable()) continuousWorker_.join();
    std::wstring fullDestination, fullSource, parent, sourceParent;
    const auto source = Path();
    if (source.empty() || !FullExportPath(destination, fullDestination, error) ||
        !FullExportPath(source, fullSource, error) ||
        !ExportParent(fullDestination, parent, error) ||
        !ExportParent(fullSource, sourceParent, error)) return false;
    const auto filename = fullDestination.substr(fullDestination.find_last_of(L"\\/") + 1);
    const auto sourceName = fullSource.substr(fullSource.find_last_of(L"\\/") + 1);
    if (_wcsicmp(parent.c_str(), sourceParent.c_str()) == 0 && IsLogFamily(filename, sourceName)) {
        error = L"不能将持续保存目标设为日志源文件或分卷"; return false;
    }
    HANDLE output = CreateFileW(fullDestination.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        error = L"无法创建持续保存目标：" + Win32ErrorMessage(); return false;
    }
    {
        std::lock_guard<std::mutex> lock(continuousMutex_);
        continuousPath_ = fullDestination;
        continuousError_.clear();
    }
    continuousStopAt_.store(0);
    continuousStop_.store(false);
    continuousActive_.store(true);
    try { continuousWorker_ = std::thread(&SessionLogger::ContinuousSaveWorker, this, output); }
    catch (...) {
        CloseHandle(output); continuousActive_.store(false);
        error = L"无法启动持续日志保存线程"; return false;
    }
    return true;
}

void SessionLogger::StopContinuousSave() {
    if (!continuousWorker_.joinable()) return;
    continuousStopAt_.store(CaptureExportPoint().bytes);
    continuousStop_.store(true);
    continuousWake_.notify_all();
    continuousWorker_.join();
    continuousActive_.store(false);
}

void SessionLogger::ContinuousSaveWorker(HANDLE output) {
    std::uint64_t offset = 0;
    std::wstring error;
    for (;;) {
        const auto point = CaptureExportPoint();
        auto end = point.bytes;
        if (continuousStop_.load()) end = std::min(end, continuousStopAt_.load());
        if (end > offset) {
            if (!CopyCompletedRange({point.generation, end, point.root}, offset, output, error))
                break;
            offset = end;
            if (!FlushFileBuffers(output)) {
                error = L"持续保存刷新目标文件失败：" + Win32ErrorMessage(); break;
            }
        }
        if (continuousStop_.load() && offset >= continuousStopAt_.load()) break;
        std::unique_lock<std::mutex> lock(continuousMutex_);
        continuousWake_.wait_for(lock, std::chrono::milliseconds(200));
    }
    if (!FlushFileBuffers(output) && error.empty())
        error = L"持续保存最终刷新失败：" + Win32ErrorMessage();
    CloseHandle(output);
    {
        std::lock_guard<std::mutex> lock(continuousMutex_);
        if (!error.empty()) continuousError_ = error;
    }
    continuousActive_.store(false);
}

std::wstring SessionLogger::Error()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_;
}

void SessionLogger::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        wake_.notify_all();
    }
    if (worker_.joinable())
        worker_.join();
    HANDLE file = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        file = file_;
        file_ = INVALID_HANDLE_VALUE;
    }
    std::wstring failure;
    if (file != INVALID_HANDLE_VALUE) {
        bool flushOk = FlushFileBuffers(file) != FALSE;
#ifdef SERIALCTL_LOGGER_TESTING
        if (testFailFlushStop_.load()) flushOk = false;
#endif
        if (!flushOk) failure = L"日志刷新失败：" + Win32ErrorMessage();
        CloseHandle(file);
    }
    if (!failure.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_.empty()) failure_ = std::move(failure);
        wake_.notify_all();
    }
}

// Called with mutex held. Producers never wait for the disk.
void SessionLogger::WriteBytes(const void *data, DWORD size)
{
    if (stopping_ || !failure_.empty() || !size)
        return;
    if (queued_ + size > 8 * 1024 * 1024)
    {
        failure_ = L"日志磁盘处理过慢，日志队列已满。";
        ++overflowEvents_;
        wake_.notify_all();
        return;
    }
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    if (!queue_.empty() && queue_.back().size() + size <= 256 * 1024)
        queue_.back().insert(queue_.back().end(), bytes, bytes + size);
    else
        queue_.emplace_back(bytes, bytes + size);
    queued_ += size;
    highWaterQueued_ = std::max(highWaterQueued_, queued_);
    accepted_ += size;
    wake_.notify_all();
}
void SessionLogger::Run()
{
    try {
    for (;;)
    {
        Bytes bytes;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty())
                break;
            bytes = std::move(queue_.front());
            queue_.pop_front();
            queued_ -= bytes.size();
            writing_ = true;
        }
        size_t offset = 0;
        std::wstring failure;
        if (written_ && written_ + bytes.size() > 64ull * 1024 * 1024)
        {
            if (!FlushFileBuffers(file_))
                failure = L"日志刷新失败：" + Win32ErrorMessage();
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            const auto dot = path_.find_last_of(L'.');
            std::wstring part = path_.substr(0, dot) + L".part" +
                                std::to_wstring(parts_.size() + 1) + path_.substr(dot);
            if (failure.empty())
                file_ = CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file_ == INVALID_HANDLE_VALUE && failure.empty())
                failure = L"日志分卷失败：" + Win32ErrorMessage();
            if (failure.empty())
            {
                std::lock_guard<std::mutex> lock(mutex_);
                parts_.push_back({std::move(part), 0});
                written_ = 0;
            }
        }
        while (failure.empty() && offset < bytes.size())
        {
            DWORD written = 0;
            if (!WriteFile(file_, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset),
                           &written, nullptr) ||
                written == 0)
            {
                failure = L"日志写入失败：" + Win32ErrorMessage();
                break;
            }
            offset += written;
            written_ += written;
        }
        bool terminal = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            writing_ = false;
            parts_.back().length += offset;
            completed_ += offset;
            if (!failure.empty())
            {
                failure_ = failure;
                queue_.clear();
                queued_ = 0;
                terminal = true;
            }
            wake_.notify_all();
        }
        // A write failure is terminal: leave the loop so writerAlive_ is cleared and
        // waiting exporters are released instead of idling until Stop().
        if (terminal)
            break;
    }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_.empty()) failure_ = L"日志后台线程异常退出";
        writing_ = false;
        queue_.clear(); queued_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        writerAlive_ = false;
        wake_.notify_all();
    }
}
} // namespace serialctl
