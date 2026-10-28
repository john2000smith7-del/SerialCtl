#pragma once
#include <windows.h>
#include <objbase.h>
#include <string>
#include <vector>
namespace serialctl {
inline std::wstring ExtendedFilePath(const std::wstring& path) {
    if (path.size() < 248 || path.rfind(L"\\\\?\\", 0) == 0) return path;
    std::vector<wchar_t> full(32768);
    const DWORD size = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!size || size >= full.size()) return {};
    std::wstring result(full.data(), size);
    return result.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + result.substr(2) : L"\\\\?\\" + result;
}
inline std::wstring ModuleFilePath() {
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return size && size < path.size() ? std::wstring(path.data(), size) : std::wstring();
}
inline bool CreateUniqueTemporaryFile(const std::wstring& directory, std::wstring& path) {
    GUID guid{}; wchar_t token[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, token, 40)) return false;
    const bool slash = !directory.empty() && (directory.back() == L'\\' || directory.back() == L'/');
    path = ExtendedFilePath(directory + (slash ? L"" : L"\\") + L"SCT-" + token + L".tmp");
    if (path.empty()) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file); return true;
}
} // namespace serialctl
