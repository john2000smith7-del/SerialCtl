#include "TftpFileSource.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <utility>
#include <windows.h>

namespace serialctl {
namespace {

struct SourceHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~SourceHandle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
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
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
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
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
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

LocalTftpFileSource::LocalTftpFileSource(std::wstring path)
    : path_(std::move(path)) {
    info_.type = "local";
    info_.displayPath = path_;
    info_.status = "Failed";
    bool notFound = false;
    SourceHandle file;
    file.value = OpenLocalSource(path_, &notFound);
    BY_HANDLE_FILE_INFORMATION snapshot{};
    if (file.value == INVALID_HANDLE_VALUE || !ReadSnapshot(file.value, snapshot)) {
        info_.notFound = (file.value == INVALID_HANDLE_VALUE) && notFound;
        info_.error = info_.notFound ? L"本地文件不存在" : L"无法读取常规文件，或路径包含重解析点";
        return;
    }
    info_.size = FileSize(snapshot);
    info_.sizeKnown = true;
    info_.status = "Ready";
}

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

} // namespace serialctl
