#include "OperationAuditLogger.h"
#include "SessionLogger.h"
#include "Win32Helpers.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <shlobj.h>

namespace serialctl {
namespace {

std::mutex& AuditRegistryMutex() {
    static auto* m = new std::mutex();
    return *m;
}

std::vector<OperationAuditLogger*>& AuditRegistry() {
    static auto* r = new std::vector<OperationAuditLogger*>();
    return *r;
}

std::wstring DefaultOperationsDirectory() {
    const std::wstring base = SessionLogger::DefaultLogDirectory();
    if (base.empty()) return {};
    const std::wstring dir = base + L"\\operations";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

} // namespace

OperationAuditLogger& OperationAuditLogger::Instance() {
    static auto* instance = new OperationAuditLogger();
    return *instance;
}

OperationAuditLogger::OperationAuditLogger() {
    std::lock_guard<std::mutex> lock(AuditRegistryMutex());
    AuditRegistry().push_back(this);
}

OperationAuditLogger::~OperationAuditLogger() {
    {
        std::lock_guard<std::mutex> lock(AuditRegistryMutex());
        auto& reg = AuditRegistry();
        reg.erase(std::remove(reg.begin(), reg.end(), this), reg.end());
    }
    Shutdown();
}

void OperationAuditLogger::Configure(const std::wstring& directory, size_t maxFiles,
                                     std::uint64_t maxFileBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
    directory_ = directory;
    maxFiles_ = (std::max<size_t>)(1, maxFiles);
    maxFileBytes_ = (std::max<std::uint64_t>)(1024, maxFileBytes);
    activePath_.clear();
    activeBytes_ = 0;
}

std::wstring OperationAuditLogger::Directory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return directory_.empty() ? DefaultOperationsDirectory() : directory_;
}

std::string OperationAuditLogger::CurrentTimestampUtf8() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[40]{};
    sprintf_s(buf, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

std::vector<std::wstring> OperationAuditLogger::EnumeratePartsLocked() const {
    std::vector<std::wstring> files;
    const std::wstring dir = directory_.empty() ? DefaultOperationsDirectory() : directory_;
    if (dir.empty()) return files;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\operations-*.jsonl").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            files.push_back(dir + L"\\" + fd.cFileName);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(files.begin(), files.end());
    return files;
}

void OperationAuditLogger::PruneOldFilesLocked() {
    auto files = EnumeratePartsLocked();
    while (files.size() > maxFiles_) {
        if (files.front() == activePath_) break;
        DeleteFileW(files.front().c_str());
        files.erase(files.begin());
    }
}

bool OperationAuditLogger::EnsureOpenLocked(std::wstring* error) {
    if (file_ != INVALID_HANDLE_VALUE) return true;
    if (directory_.empty()) directory_ = DefaultOperationsDirectory();
    if (directory_.empty()) {
        if (error) *error = L"无法定位操作审计日志目录";
        return false;
    }
    if (SHCreateDirectoryExW(nullptr, directory_.c_str(), nullptr) != ERROR_SUCCESS &&
        GetFileAttributesW(directory_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (error) *error = L"无法创建操作审计日志目录：" + Win32ErrorMessage();
        return false;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[96]{};
    swprintf_s(name, L"\\operations-%04u%02u%02u-%02u%02u%02u-%04llu.jsonl",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               static_cast<unsigned long long>(++sequence_));
    const std::wstring candidate = directory_ + name;
    HANDLE h = CreateFileW(candidate.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = L"无法创建操作审计日志文件：" + Win32ErrorMessage();
        return false;
    }
    file_ = h;
    activeHandle_.store(h);
    activePath_ = candidate;
    activeBytes_ = 0;
    PruneOldFilesLocked();
    return true;
}

void OperationAuditLogger::CloseLocked() {
    if (file_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(file_);
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        activeHandle_.store(INVALID_HANDLE_VALUE);
    }
}

void OperationAuditLogger::Record(const std::string& category, const std::string& action,
                                  const std::string& target, const std::string& summary,
                                  const std::string& result) {
    nlohmann::json entry = {
        {"timestamp", CurrentTimestampUtf8()},
        {"category", category},
        {"action", action},
        {"target", target},
        {"summary", summary},
        {"result", result}
    };
    const std::string line = entry.dump() + "\r\n";

    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureOpenLocked()) return;
    if (activeBytes_ > 0 && activeBytes_ + line.size() > maxFileBytes_) {
        CloseLocked();
        if (!EnsureOpenLocked()) return;
    }
    DWORD offset = 0;
    const DWORD total = static_cast<DWORD>(line.size());
    while (offset < total) {
        DWORD written = 0;
        if (!WriteFile(file_, line.data() + offset, total - offset, &written, nullptr) || !written) {
            break;
        }
        offset += written;
    }
    activeBytes_ += offset;
    if (offset == total) ++recordedCount_;
}

void OperationAuditLogger::Flush() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (lock.owns_lock()) {
        if (file_ != INVALID_HANDLE_VALUE) FlushFileBuffers(file_);
        return;
    }
    const HANDLE handle = activeHandle_.load();
    if (handle != INVALID_HANDLE_VALUE) FlushFileBuffers(handle);
}

void OperationAuditLogger::EmergencyFlushAll() {
    std::unique_lock<std::mutex> lock(AuditRegistryMutex(), std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (OperationAuditLogger* logger : AuditRegistry()) {
        if (logger) logger->Flush();
    }
}

bool OperationAuditLogger::ExportTo(const std::wstring& destinationFile, std::wstring& error) const {
    if (destinationFile.empty()) {
        error = L"导出文件路径为空";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) FlushFileBuffers(file_);
    const auto files = EnumeratePartsLocked();
    const std::wstring temp = destinationFile + L".serialctl-tmp";
    HANDLE out = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        error = L"无法创建操作日志导出文件：" + Win32ErrorMessage();
        return false;
    }
    bool ok = true;
    std::array<char, 65536> buf{};
    for (const auto& srcPath : files) {
        HANDLE in = CreateFileW(srcPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (in == INVALID_HANDLE_VALUE) continue;
        DWORD readBytes = 0;
        while (ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &readBytes, nullptr) && readBytes > 0) {
            DWORD off = 0;
            while (off < readBytes) {
                DWORD wrote = 0;
                if (!WriteFile(out, buf.data() + off, readBytes - off, &wrote, nullptr) || !wrote) {
                    error = L"写入操作日志导出文件失败：" + Win32ErrorMessage();
                    ok = false;
                    break;
                }
                off += wrote;
            }
            if (!ok) break;
        }
        CloseHandle(in);
        if (!ok) break;
    }
    if (ok && !FlushFileBuffers(out)) {
        error = L"刷新操作日志导出文件失败：" + Win32ErrorMessage();
        ok = false;
    }
    CloseHandle(out);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), destinationFile.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"保存操作日志导出文件失败：" + Win32ErrorMessage();
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool OperationAuditLogger::ClearHistory(std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
    activePath_.clear();
    activeBytes_ = 0;
    const auto files = EnumeratePartsLocked();
    for (const auto& f : files) {
        if (!DeleteFileW(f.c_str())) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND) {
                error = L"删除历史操作日志失败：" + Win32ErrorMessage(code);
                return false;
            }
        }
    }
    recordedCount_ = 0;
    return true;
}

void OperationAuditLogger::SetAutoDeleteOnExit(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    autoDeleteOnExit_ = enabled;
}

bool OperationAuditLogger::AutoDeleteOnExit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return autoDeleteOnExit_;
}

void OperationAuditLogger::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
    activePath_.clear();
    activeBytes_ = 0;
    if (autoDeleteOnExit_) {
        for (const auto& f : EnumeratePartsLocked()) {
            DeleteFileW(f.c_str());
        }
    }
}

std::uint64_t OperationAuditLogger::RecordedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recordedCount_;
}

} // namespace serialctl
