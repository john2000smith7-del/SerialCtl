#pragma once

#include <windows.h>

#include <sstream>
#include <string>
#include <vector>

namespace serialctl {

inline std::wstring Win32ErrorMessage(DWORD code = GetLastError()) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::wstring result = length && buffer ? std::wstring(buffer, length) : L"未知错误";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

inline std::wstring SocketErrorMessage(int code) {
    return Win32ErrorMessage(static_cast<DWORD>(code));
}

inline std::string WideToMultiByte(const std::wstring& text, UINT codePage) {
    if (text.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(
        codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(
        codePage, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

inline std::wstring MultiByteToWide(const std::uint8_t* data, size_t size, UINT codePage) {
    if (!data || size == 0) {
        return {};
    }
    const DWORD flags = codePage == CP_UTF8 ? 0 : 0;
    const int count = MultiByteToWideChar(
        codePage, flags, reinterpret_cast<const char*>(data), static_cast<int>(size), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(
        codePage, flags, reinterpret_cast<const char*>(data), static_cast<int>(size), result.data(), count);
    return result;
}

inline std::wstring QuoteCommandLineArgument(const std::wstring& value) {
    std::wstring output = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            output.append(backslashes * 2 + 1, L'\\');
            output.push_back(L'\"');
            backslashes = 0;
        } else {
            output.append(backslashes, L'\\');
            backslashes = 0;
            output.push_back(ch);
        }
    }
    output.append(backslashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

} // namespace serialctl
