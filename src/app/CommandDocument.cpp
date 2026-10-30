#include "CommandDocument.h"
#include "FilePaths.h"
#include "Win32Helpers.h"
#include <cstdio>
#include <io.h>
namespace serialctl {
bool ReadCommandDocument(const std::wstring &path, std::wstring &text, std::wstring &error) {
    FILE *f = nullptr;
    if (_wfopen_s(&f, ExtendedFilePath(path).c_str(), L"rb") || !f) {
        error = L"无法读取命令文件，原内容已保留";
        return false;
    }
    fseek(f, 0, SEEK_END);
    const auto size = ftell(f);
    rewind(f);
    if (size < 0 || size > 4 * 1024 * 1024) {
        fclose(f);
        error = L"命令文件不能超过 4 MB";
        return false;
    }
    std::string raw(static_cast<size_t>(size), '\0');
    const bool read = raw.empty() || fread(raw.data(), 1, raw.size(), f) == raw.size();
    fclose(f);
    if (!read) {
        error = L"命令文件读取不完整";
        return false;
    }
    if (raw.rfind("\xEF\xBB\xBF", 0) == 0)
        raw.erase(0, 3);
    if (raw.find('\0') != std::string::npos ||
        (!raw.empty() &&
         !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0))) {
        error = L"请使用 UTF-8 文本文件（不能包含 NUL）";
        return false;
    }
    text = MultiByteToWide(reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size(), CP_UTF8);
    return true;
}
bool WriteCommandDocument(const std::wstring &path, const std::wstring &text, std::wstring &error) {
    const auto destination = ExtendedFilePath(path, true);
    const auto slash = destination.find_last_of(L"\\/");
    std::wstring temporary;
    if (slash == std::wstring::npos || !CreateUniqueTemporaryFile(destination.substr(0, slash), temporary)) {
        error = L"无法创建命令文件临时副本";
        return false;
    }
    std::wstring normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n')
                ++i;
            normalized += L"\r\n";
        } else if (text[i] == L'\n')
            normalized += L"\r\n";
        else
            normalized += text[i];
    }
    const auto raw = WideToMultiByte(normalized, CP_UTF8);
    FILE *f = nullptr;
    if (_wfopen_s(&f, temporary.c_str(), L"wb") || !f) {
        DeleteFileW(temporary.c_str());
        error = L"无法写入命令文件";
        return false;
    }
    bool ok = raw.empty() || fwrite(raw.data(), 1, raw.size(), f) == raw.size();
    if (fflush(f) || _commit(_fileno(f)))
        ok = false;
    if (fclose(f))
        ok = false;
    if (!ok ||
        !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = L"保存失败，原文件已保留";
        return false;
    }
    return true;
}
} // namespace serialctl
