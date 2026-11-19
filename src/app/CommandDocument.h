#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace serialctl {
// Positions are native UTF-16 offsets, including either CR, LF or CRLF endings.
struct CommandLine {
    size_t start = 0, end = 0, next = 0;
};
inline std::vector<CommandLine> CommandLines(const std::wstring &text) {
    std::vector<CommandLine> result;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'\r' && text[i] != L'\n')
            continue;
        size_t next = i + 1;
        if (text[i] == L'\r' && next < text.size() && text[next] == L'\n')
            ++next;
        result.push_back({start, i, next});
        start = next;
        i = next - 1;
    }
    result.push_back({start, text.size(), text.size()});
    return result;
}
inline std::pair<size_t, size_t> CommandSelectedLines(const std::vector<CommandLine> &lines, size_t a, size_t b) {
    if (a > b)
        std::swap(a, b);
    auto index = [&](size_t p) {
        size_t i = 0;
        while (i + 1 < lines.size() && p >= lines[i].next)
            ++i;
        return i;
    };
    return {index(a), index(a == b ? b : b - 1)};
}
inline std::wstring StripCommandRemark(std::wstring command) {
    wchar_t quote = 0;
    for (size_t i = 0; i < command.size(); ++i) {
        const wchar_t ch = command[i];
        if (ch == L'\\' && i + 1 < command.size() &&
            (command[i + 1] == L'\\' || command[i + 1] == L'"' ||
             command[i + 1] == L'\'' || command[i + 1] == L'#' || command[i + 1] == L'/')) {
            ++i;
            continue;
        }
        if (quote) {
            if (ch == quote) quote = 0;
            continue;
        }
        if (ch == L'"' || ch == L'\'') {
            quote = ch;
            continue;
        }
        const bool boundary = i == 0 || command[i - 1] == L' ' || command[i - 1] == L'\t';
        const bool slashComment = ch == L'/' && i + 1 < command.size() &&
            (command[i + 1] == L'/' || command[i + 1] == L'#');
        if (boundary && (ch == L'#' || slashComment)) {
            const auto end = command.find_last_not_of(L" \t", i == 0 ? 0 : i - 1);
            command.resize(i == 0 || end == std::wstring::npos ? 0 : end + 1);
            break;
        }
    }
    return command;
}
inline std::wstring CommandLineText(const std::wstring &text, const CommandLine &row) {
    auto s = text.substr(row.start, row.end - row.start);
    const auto first = s.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return {};
    const auto content = s.substr(first);
    if (content.front() == L'#' || content.rfind(L"[[macro", 0) == 0 || content == L"[[/macro]]")
        return {};
    s = StripCommandRemark(std::move(s));
    // Old command files escape literal comment/marker commands with a backslash.
    if (content.rfind(L"\\#", 0) == 0 || content.rfind(L"\\[[", 0) == 0)
        s.erase(first, 1);
    for (const wchar_t ch : s)
        if (ch < 32 && ch != L'\t')
            return {};
    return s;
}
inline std::vector<std::wstring> CommandSelection(const std::wstring &text, size_t a, size_t b) {
    const auto lines = CommandLines(text);
    const auto range = CommandSelectedLines(lines, a, b);
    std::vector<std::wstring> result;
    for (size_t i = range.first; i <= range.second; ++i) {
        auto s = CommandLineText(text, lines[i]);
        if (!s.empty())
            result.push_back(std::move(s));
    }
    return result;
}
bool ReadCommandDocument(const std::wstring &path, std::wstring &text, std::wstring &error);
bool WriteCommandDocument(const std::wstring &path, const std::wstring &text, std::wstring &error);
} // namespace serialctl
