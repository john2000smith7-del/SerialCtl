#include "SessionLogger.h"
#include "Win32Helpers.h"

#include <shlobj.h>
#include <array>

namespace serialctl {

SessionLogger::~SessionLogger() {
    Stop();
}

bool SessionLogger::Start(const std::wstring& mode, std::wstring& error) {
    Stop();
    path_.clear();
    std::array<wchar_t, MAX_PATH> appData{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData.data()))) {
        error = L"无法找到本地应用数据目录";
        return false;
    }
    std::wstring directory = std::wstring(appData.data()) + L"\\SerialCtl\\logs";
    if (SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr) != ERROR_SUCCESS &&
        GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"无法创建日志目录：" + Win32ErrorMessage();
        return false;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t baseName[64]{};
    swprintf_s(baseName, L"\\%04u%02u%02u-%02u%02u%02u",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    for (unsigned int suffix = 1; suffix <= 100 && file_ == INVALID_HANDLE_VALUE; ++suffix) {
        path_ = directory + baseName + (suffix == 1 ? L".log" : L"-" + std::to_wstring(suffix) + L".log");
        file_ = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (file_ == INVALID_HANDLE_VALUE) {
        error = L"无法创建日志：" + Win32ErrorMessage();
        return false;
    }
    stopping_ = false; failure_.clear();
    worker_ = std::thread(&SessionLogger::Run, this);
    WriteStatus(L"SerialCtl 会话开始，模式：" + mode);
    return true;
}

void SessionLogger::WriteRaw(const Bytes& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE && !data.empty()) {
        WriteBytes(data.data(), static_cast<DWORD>(data.size()));
    }
}

void SessionLogger::WriteText(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE || text.empty()) return;
    const std::string utf8 = WideToMultiByte(text, CP_UTF8);
    WriteBytes(utf8.data(), static_cast<DWORD>(utf8.size()));
}

void SessionLogger::WriteStatus(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t prefix[32]{};
    swprintf_s(prefix, L"\r\n[%02u:%02u:%02u.%03u] ",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    const std::string utf8 = WideToMultiByte(std::wstring(prefix) + text + L"\r\n", CP_UTF8);
    WriteBytes(utf8.data(), static_cast<DWORD>(utf8.size()));
}

bool SessionLogger::SaveCopy(const std::wstring& destination, std::wstring& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [this] { return queue_.empty() && !writing_; });
    if (!failure_.empty()) { error = failure_; return false; }
    if (path_.empty()) {
        error = L"当前没有可保存的会话日志。";
        return false;
    }
    if (file_ != INVALID_HANDLE_VALUE && !FlushFileBuffers(file_)) { failure_ = error = L"日志刷新失败：" + Win32ErrorMessage(); return false; }
    if (!CopyFileW(path_.c_str(), destination.c_str(), FALSE)) {
        error = L"无法保存日志：" + Win32ErrorMessage();
        return false;
    }
    return true;
}

std::wstring SessionLogger::Error() { std::lock_guard<std::mutex> lock(mutex_); return failure_; }

void SessionLogger::Stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; wake_.notify_all(); }
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        if (!FlushFileBuffers(file_) && failure_.empty()) failure_ = L"日志刷新失败：" + Win32ErrorMessage();
        CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
    }
}

// Called with mutex held. Producers never wait for the disk.
void SessionLogger::WriteBytes(const void* data, DWORD size) {
    if (stopping_ || !failure_.empty() || !size) return;
    if (queued_ + size > 8 * 1024 * 1024) { failure_ = L"日志磁盘处理过慢，日志队列已满。"; return; }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    queue_.emplace_back(bytes, bytes + size); queued_ += size; wake_.notify_one();
}
void SessionLogger::Run() {
    for (;;) {
        Bytes bytes;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) break;
            bytes = std::move(queue_.front()); queue_.pop_front(); queued_ -= bytes.size(); writing_ = true;
        }
        size_t offset = 0; std::wstring failure;
        while (offset < bytes.size()) {
            DWORD written = 0;
            if (!WriteFile(file_, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &written, nullptr) || written == 0) { failure = L"日志写入失败：" + Win32ErrorMessage(); break; }
            offset += written;
        }
        { std::lock_guard<std::mutex> lock(mutex_); writing_ = false; if (!failure.empty()) { failure_ = failure; queue_.clear(); queued_ = 0; } wake_.notify_all(); }
    }
}
} // namespace serialctl
