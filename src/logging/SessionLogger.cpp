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
    std::lock_guard<std::mutex> lock(mutex_);
    if (path_.empty()) {
        error = L"当前没有可保存的会话日志。";
        return false;
    }
    if (file_ != INVALID_HANDLE_VALUE) FlushFileBuffers(file_);
    if (!CopyFileW(path_.c_str(), destination.c_str(), FALSE)) {
        error = L"无法保存日志：" + Win32ErrorMessage();
        return false;
    }
    return true;
}

void SessionLogger::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

void SessionLogger::WriteBytes(const void* data, DWORD size) {
    DWORD written = 0;
    WriteFile(file_, data, size, &written, nullptr);
}

} // namespace serialctl
