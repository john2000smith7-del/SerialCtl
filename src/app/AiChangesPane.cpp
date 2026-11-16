#include "AiChangesPane.h"
#include "Win32Helpers.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwctype>
#include <sstream>
#include <string_view>
#include <thread>
#include <windowsx.h>

namespace serialctl {
namespace {

constexpr wchar_t kChangesHostClass[] = L"SerialCtlAiChangesHost";
constexpr wchar_t kChangesViewerClass[] = L"SerialCtlAiChangesViewer";

void DrawTextBox(HDC dc, const std::wstring &text, RECT rect, HFONT font, COLORREF color, UINT flags) {
    if (!dc || text.empty() || rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    const auto oldFont = font ? SelectObject(dc, font) : nullptr;
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    if (oldFont)
        SelectObject(dc, oldFont);
}

std::wstring ControlCaption(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0)
        return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

void DrawOverlayScrollbar(HDC dc, const RECT &thumb, const UiColors &colors, int dpi) {
    if (thumb.bottom <= thumb.top || thumb.right <= thumb.left)
        return;
    UiBox(dc, thumb, colors.border, colors.border, MulDiv(3, dpi, 96));
}

bool IsWordChar(wchar_t ch) {
    return (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
           ch == L'_' || ch >= 0x80;
}

std::wstring ToLowerWide(const std::wstring &s) {
    std::wstring out;
    out.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        out[i] = static_cast<wchar_t>(std::towlower(s[i]));
    return out;
}

std::vector<std::wstring> SplitUtf8LinesWide(const std::string &utf8, const std::function<std::wstring(const std::string &)> &conv) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= utf8.size()) {
        const size_t nl = utf8.find('\n', pos);
        std::string line = (nl == std::string::npos) ? utf8.substr(pos) : utf8.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(conv(line));
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
        if (pos == utf8.size())
            break;
    }
    return out;
}

bool ParseHunkHeader(const std::string &line, int &oldStart, int &oldCount, int &newStart, int &newCount) {
    if (line.rfind("@@", 0) != 0)
        return false;
    const size_t endMarker = line.find("@@", 2);
    if (endMarker == std::string::npos)
        return false;
    const std::string body = line.substr(2, endMarker - 2);
    oldStart = 1;
    oldCount = 0;
    newStart = 1;
    newCount = 0;
    int parsed = 0;
    size_t minusPos = body.find('-');
    size_t plusPos = body.find('+');
    if (minusPos != std::string::npos) {
        const char *p = body.c_str() + minusPos + 1;
        char *next = nullptr;
        oldStart = static_cast<int>(std::strtol(p, &next, 10));
        if (next && *next == ',')
            oldCount = static_cast<int>(std::strtol(next + 1, nullptr, 10));
        else
            oldCount = 1;
        ++parsed;
    }
    if (plusPos != std::string::npos) {
        const char *p = body.c_str() + plusPos + 1;
        char *next = nullptr;
        newStart = static_cast<int>(std::strtol(p, &next, 10));
        if (next && *next == ',')
            newCount = static_cast<int>(std::strtol(next + 1, nullptr, 10));
        else
            newCount = 1;
        ++parsed;
    }
    return parsed == 2;
}

std::wstring ShortActionTag(const std::string &action) {
    if (action == "create")
        return L"[增]";
    if (action == "delete")
        return L"[删]";
    if (action == "move")
        return L"[移]";
    return L"[改]";
}

void SplitDisplayPath(const std::string &pathUtf8, std::string &fileName, std::string &dirName) {
    std::string norm = pathUtf8;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    while (norm.size() > 1 && norm.back() == '/')
        norm.pop_back();
    const size_t slash = norm.find_last_of('/');
    if (slash == std::string::npos) {
        fileName = norm;
        dirName.clear();
    } else {
        fileName = norm.substr(slash + 1);
        dirName = norm.substr(0, slash);
    }
}

void DrawRoundedBorderStroke(HDC dc, const RECT &rect, int radius, COLORREF borderColor) {
    const float w = static_cast<float>(rect.right - rect.left - 1);
    const float h = static_cast<float>(rect.bottom - rect.top - 1);
    if (!dc || w <= 0 || h <= 0)
        return;
    const float x = rect.left + 0.5f;
    const float y = rect.top + 0.5f;
    const float diameter = std::min(static_cast<float>(std::max(0, radius) * 2), std::min(w, h));
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    if (diameter > 0) {
        path.AddArc(x, y, diameter, diameter, 180, 90);
        path.AddArc(x + w - diameter, y, diameter, diameter, 270, 90);
        path.AddArc(x + w - diameter, y + h - diameter, diameter, diameter, 0, 90);
        path.AddArc(x, y + h - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
    } else {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
    }
    Gdiplus::Pen pen(UiColor(borderColor), 1.0f);
    graphics.DrawPath(&pen, &path);
}

enum class SyntaxLang { Plain, Cpp, Python, PowerShell, Script, Shell, Json, Markdown, CMake, Config };

enum class SyntaxKind {
    Plain,
    Keyword,
    Control,
    Type,
    Function,
    String,
    Number,
    Comment,
    Preprocessor,
    Variable,
    Operator
};

struct SyntaxSpan {
    size_t start = 0;
    size_t end = 0;
    SyntaxKind kind = SyntaxKind::Plain;
};

std::wstring ExpandTabs4(const std::wstring &text) {
    if (text.find(L'\t') == std::wstring::npos)
        return text;
    std::wstring expanded;
    expanded.reserve(text.size() + 8);
    for (wchar_t ch : text) {
        if (ch == L'\t') {
            const size_t spaces = 4 - (expanded.size() % 4);
            expanded.append(spaces, L' ');
        } else {
            expanded.push_back(ch);
        }
    }
    return expanded;
}

SyntaxLang DetectSyntaxLanguage(const std::string &pathUtf8) {
    std::string fileName, dirName;
    SplitDisplayPath(pathUtf8, fileName, dirName);
    std::string lowerName = fileName;
    for (char &ch : lowerName)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lowerName == "cmakelists.txt")
        return SyntaxLang::CMake;
    const size_t dot = lowerName.find_last_of('.');
    if (dot == std::string::npos)
        return SyntaxLang::Plain;
    const std::string ext = lowerName.substr(dot);
    if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" || ext == ".hh" ||
        ext == ".hpp" || ext == ".hxx" || ext == ".inl" || ext == ".rc")
        return SyntaxLang::Cpp;
    if (ext == ".py" || ext == ".pyw" || ext == ".pyi")
        return SyntaxLang::Python;
    if (ext == ".ps1" || ext == ".psm1" || ext == ".psd1")
        return SyntaxLang::PowerShell;
    if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs" || ext == ".ts" || ext == ".tsx" ||
        ext == ".java" || ext == ".cs" || ext == ".go" || ext == ".rs")
        return SyntaxLang::Script;
    if (ext == ".sh" || ext == ".bash" || ext == ".zsh")
        return SyntaxLang::Shell;
    if (ext == ".json" || ext == ".jsonc")
        return SyntaxLang::Json;
    if (ext == ".md" || ext == ".markdown")
        return SyntaxLang::Markdown;
    if (ext == ".cmake")
        return SyntaxLang::CMake;
    if (ext == ".ini" || ext == ".cfg" || ext == ".conf" || ext == ".yaml" || ext == ".yml" ||
        ext == ".toml" || ext == ".xml" || ext == ".html")
        return SyntaxLang::Config;
    return SyntaxLang::Plain;
}

COLORREF SyntaxTokenColor(SyntaxKind kind, bool dark, const UiColors &colors) {
    if (dark) {
        switch (kind) {
        case SyntaxKind::Keyword: return RGB(86, 156, 214);
        case SyntaxKind::Control: return RGB(197, 134, 192);
        case SyntaxKind::Type: return RGB(78, 201, 176);
        case SyntaxKind::Function: return RGB(220, 220, 170);
        case SyntaxKind::String: return RGB(206, 145, 120);
        case SyntaxKind::Number: return RGB(181, 206, 168);
        case SyntaxKind::Comment: return RGB(106, 153, 85);
        case SyntaxKind::Preprocessor: return RGB(197, 134, 192);
        case SyntaxKind::Variable: return RGB(156, 220, 254);
        case SyntaxKind::Operator: return RGB(178, 186, 198);
        case SyntaxKind::Plain:
        default: return RGB(212, 212, 212);
        }
    }
    switch (kind) {
    case SyntaxKind::Keyword: return RGB(0, 0, 255);
    case SyntaxKind::Control: return RGB(175, 0, 219);
    case SyntaxKind::Type: return RGB(38, 127, 153);
    case SyntaxKind::Function: return RGB(121, 94, 38);
    case SyntaxKind::String: return RGB(163, 21, 21);
    case SyntaxKind::Number: return RGB(9, 134, 88);
    case SyntaxKind::Comment: return RGB(0, 128, 0);
    case SyntaxKind::Preprocessor: return RGB(175, 0, 219);
    case SyntaxKind::Variable: return RGB(0, 16, 128);
    case SyntaxKind::Operator: return RGB(56, 58, 66);
    case SyntaxKind::Plain:
    default: return colors.text;
    }
}

bool ContainsWordCI(std::wstring_view word, const std::initializer_list<std::wstring_view> &list) {
    for (const auto item : list) {
        if (item.size() == word.size() &&
            std::equal(word.begin(), word.end(), item.begin(), [](wchar_t a, wchar_t b) {
                return std::towlower(a) == std::towlower(b);
            }))
            return true;
    }
    return false;
}

bool ContainsWord(std::wstring_view word, const std::initializer_list<std::wstring_view> &list) {
    for (const auto item : list) {
        if (item == word)
            return true;
    }
    return false;
}

bool IsIdentStart(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') || ch == L'_';
}

bool IsIdentBody(wchar_t ch) {
    return IsIdentStart(ch) || (ch >= L'0' && ch <= L'9');
}

bool LooksLikeTypeName(std::wstring_view w) {
    if (ContainsWord(w, {L"std", L"size_t", L"ptrdiff_t", L"intptr_t", L"uintptr_t", L"uint8_t", L"uint16_t",
                         L"uint32_t", L"uint64_t", L"int8_t", L"int16_t", L"int32_t", L"int64_t", L"string",
                         L"wstring", L"string_view", L"wstring_view", L"vector", L"array", L"map", L"set",
                         L"unordered_map", L"unique_ptr", L"shared_ptr", L"optional", L"variant", L"function",
                         L"atomic", L"mutex", L"lock_guard", L"HWND", L"HDC", L"HFONT", L"HBRUSH", L"HBITMAP",
                         L"HGDIOBJ", L"HINSTANCE", L"HMENU", L"HANDLE", L"RECT", L"POINT", L"SIZE", L"COLORREF",
                         L"BOOL", L"BYTE", L"WORD", L"DWORD", L"UINT", L"LONG", L"LRESULT", L"WPARAM", L"LPARAM",
                         L"INT_PTR", L"UINT_PTR", L"DWORD_PTR", L"Json", L"UiColors", L"UiFieldStyle"}))
        return true;
    if (w.size() >= 2 && w[0] >= L'A' && w[0] <= L'Z') {
        for (size_t i = 1; i < w.size(); ++i) {
            if (w[i] >= L'a' && w[i] <= L'z')
                return true;
        }
    }
    return false;
}

SyntaxKind ClassifyIdentifier(std::wstring_view word, SyntaxLang lang, bool followedByParen) {
    if (lang == SyntaxLang::Cpp || lang == SyntaxLang::Script) {
        if (ContainsWord(word, {L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default", L"break",
                                L"continue", L"return", L"goto", L"try", L"catch", L"throw", L"finally", L"co_await",
                                L"co_return", L"co_yield", L"yield", L"await", L"async", L"match", L"export",
                                L"import", L"from"}))
            return SyntaxKind::Control;
        if (ContainsWord(word, {L"auto", L"bool", L"char", L"char8_t", L"char16_t", L"char32_t", L"wchar_t", L"short",
                                L"int", L"long", L"signed", L"unsigned", L"float", L"double", L"void", L"const",
                                L"constexpr", L"consteval", L"constinit", L"static", L"extern", L"inline", L"virtual",
                                L"override", L"final", L"explicit", L"mutable", L"volatile", L"noexcept", L"class",
                                L"struct", L"enum", L"union", L"namespace", L"using", L"template", L"typename",
                                L"public", L"private", L"protected", L"friend", L"operator", L"sizeof", L"alignof",
                                L"decltype", L"nullptr", L"true", L"false", L"this", L"new", L"delete",
                                L"static_cast", L"reinterpret_cast", L"const_cast", L"dynamic_cast", L"function",
                                L"let", L"var", L"fn", L"pub", L"mut", L"impl", L"trait", L"type", L"interface",
                                L"extends", L"implements", L"package", L"func", L"defer", L"select", L"go", L"chan",
                                L"null", L"undefined", L"self", L"super"}))
            return SyntaxKind::Keyword;
        if (followedByParen)
            return SyntaxKind::Function;
        if (LooksLikeTypeName(word))
            return SyntaxKind::Type;
        return SyntaxKind::Plain;
    }
    if (lang == SyntaxLang::Python) {
        if (ContainsWord(word, {L"if", L"elif", L"else", L"for", L"while", L"break", L"continue", L"return",
                                L"yield", L"try", L"except", L"finally", L"raise", L"with", L"as", L"pass",
                                L"import", L"from", L"async", L"await", L"match", L"case"}))
            return SyntaxKind::Control;
        if (ContainsWord(word, {L"def", L"class", L"lambda", L"global", L"nonlocal", L"assert", L"del", L"in",
                                L"is", L"not", L"and", L"or", L"True", L"False", L"None", L"self", L"cls"}))
            return SyntaxKind::Keyword;
        if (followedByParen)
            return SyntaxKind::Function;
        if (LooksLikeTypeName(word))
            return SyntaxKind::Type;
        return SyntaxKind::Plain;
    }
    if (lang == SyntaxLang::PowerShell) {
        if (ContainsWordCI(word, {L"if", L"elseif", L"else", L"for", L"foreach", L"while", L"do", L"until",
                                  L"switch", L"break", L"continue", L"return", L"try", L"catch", L"finally",
                                  L"throw", L"trap", L"exit"}))
            return SyntaxKind::Control;
        if (ContainsWordCI(word, {L"function", L"filter", L"param", L"begin", L"process", L"end", L"class", L"enum",
                                  L"using", L"true", L"false", L"null"}))
            return SyntaxKind::Keyword;
        if (followedByParen)
            return SyntaxKind::Function;
        return SyntaxKind::Plain;
    }
    if (lang == SyntaxLang::Shell) {
        if (ContainsWord(word, {L"if", L"then", L"elif", L"else", L"fi", L"for", L"in", L"do", L"done", L"while",
                                L"until", L"case", L"esac", L"break", L"continue", L"return", L"exit"}))
            return SyntaxKind::Control;
        if (ContainsWord(word, {L"function", L"local", L"export", L"readonly", L"declare", L"typeset", L"source",
                                L"true", L"false"}))
            return SyntaxKind::Keyword;
        if (followedByParen)
            return SyntaxKind::Function;
        return SyntaxKind::Plain;
    }
    if (lang == SyntaxLang::Json) {
        if (ContainsWord(word, {L"true", L"false", L"null"}))
            return SyntaxKind::Keyword;
        return SyntaxKind::Plain;
    }
    if (lang == SyntaxLang::CMake) {
        if (ContainsWordCI(word, {L"if", L"elseif", L"else", L"endif", L"foreach", L"endforeach", L"while",
                                  L"endwhile", L"break", L"continue", L"return"}))
            return SyntaxKind::Control;
        if (ContainsWordCI(word, {L"function", L"endfunction", L"macro", L"endmacro", L"set", L"unset", L"option",
                                  L"message", L"project", L"cmake_minimum_required", L"add_executable",
                                  L"add_library", L"target_link_libraries", L"target_include_directories",
                                  L"target_compile_definitions", L"target_compile_options", L"add_test",
                                  L"include", L"find_package", L"install", L"file", L"list", L"string",
                                  L"PRIVATE", L"PUBLIC", L"INTERFACE", L"REQUIRED", L"ON", L"OFF", L"TRUE", L"FALSE"}))
            return SyntaxKind::Keyword;
        if (followedByParen)
            return SyntaxKind::Function;
        return SyntaxKind::Plain;
    }
    if (lang == SyntaxLang::Config) {
        if (ContainsWordCI(word, {L"true", L"false", L"yes", L"no", L"on", L"off", L"null"}))
            return SyntaxKind::Keyword;
        return SyntaxKind::Plain;
    }
    return followedByParen ? SyntaxKind::Function : SyntaxKind::Plain;
}

std::vector<SyntaxSpan> TokenizeSyntaxLine(const std::wstring &line, SyntaxLang lang, bool &inBlockComment) {
    std::vector<SyntaxSpan> spans;
    if (line.empty())
        return spans;

    auto emit = [&](size_t s, size_t e, SyntaxKind k) {
        if (e <= s)
            return;
        if (!spans.empty() && spans.back().end == s && spans.back().kind == k)
            spans.back().end = e;
        else
            spans.push_back({s, e, k});
    };

    const size_t n = line.size();
    size_t i = 0;

    if (lang == SyntaxLang::Markdown) {
        size_t leading = 0;
        while (leading < n && line[leading] == L' ')
            ++leading;
        if (leading < n && line[leading] == L'#') {
            emit(0, n, SyntaxKind::Keyword);
            return spans;
        }
        while (i < n) {
            if (line[i] == L'`') {
                size_t j = i + 1;
                while (j < n && line[j] != L'`')
                    ++j;
                if (j < n)
                    ++j;
                emit(i, j, SyntaxKind::String);
                i = j;
                continue;
            }
            if (line[i] == L'[' || line[i] == L']' || line[i] == L'*') {
                emit(i, i + 1, SyntaxKind::Control);
                ++i;
                continue;
            }
            size_t j = i + 1;
            while (j < n && line[j] != L'`' && line[j] != L'[' && line[j] != L']' && line[j] != L'*')
                ++j;
            emit(i, j, SyntaxKind::Plain);
            i = j;
        }
        return spans;
    }

    if (inBlockComment && (lang == SyntaxLang::Cpp || lang == SyntaxLang::Script || lang == SyntaxLang::PowerShell)) {
        const wchar_t *closeToken = (lang == SyntaxLang::PowerShell) ? L"#>" : L"*/";
        const size_t closePos = line.find(closeToken);
        if (closePos == std::wstring::npos) {
            emit(0, n, SyntaxKind::Comment);
            return spans;
        }
        emit(0, closePos + 2, SyntaxKind::Comment);
        inBlockComment = false;
        i = closePos + 2;
    }

    if ((lang == SyntaxLang::Cpp || lang == SyntaxLang::Script) && i == 0) {
        size_t ws = 0;
        while (ws < n && line[ws] == L' ')
            ++ws;
        if (lang == SyntaxLang::Cpp && ws < n && line[ws] == L'#') {
            size_t dirEnd = ws + 1;
            while (dirEnd < n && line[dirEnd] == L' ')
                ++dirEnd;
            while (dirEnd < n && IsIdentBody(line[dirEnd]))
                ++dirEnd;
            emit(0, ws, SyntaxKind::Plain);
            emit(ws, dirEnd, SyntaxKind::Preprocessor);
            i = dirEnd;
            const std::wstring_view directive(line.data() + ws, dirEnd - ws);
            if (directive.find(L"include") != std::wstring_view::npos) {
                while (i < n && line[i] == L' ')
                    ++i;
                if (i < n && (line[i] == L'<' || line[i] == L'"')) {
                    const wchar_t closer = (line[i] == L'<') ? L'>' : L'"';
                    size_t endStr = i + 1;
                    while (endStr < n && line[endStr] != closer)
                        ++endStr;
                    if (endStr < n)
                        ++endStr;
                    emit(dirEnd, i, SyntaxKind::Plain);
                    emit(i, endStr, SyntaxKind::String);
                    i = endStr;
                }
            }
        }
    }

    while (i < n) {
        const wchar_t ch = line[i];

        if ((lang == SyntaxLang::Cpp || lang == SyntaxLang::Script || lang == SyntaxLang::Json) &&
            ch == L'/' && i + 1 < n && line[i + 1] == L'/') {
            emit(i, n, SyntaxKind::Comment);
            break;
        }
        if ((lang == SyntaxLang::Cpp || lang == SyntaxLang::Script) &&
            ch == L'/' && i + 1 < n && line[i + 1] == L'*') {
            const size_t closePos = line.find(L"*/", i + 2);
            if (closePos == std::wstring::npos) {
                emit(i, n, SyntaxKind::Comment);
                inBlockComment = true;
                break;
            }
            emit(i, closePos + 2, SyntaxKind::Comment);
            i = closePos + 2;
            continue;
        }
        if (lang == SyntaxLang::PowerShell && ch == L'<' && i + 1 < n && line[i + 1] == L'#') {
            const size_t closePos = line.find(L"#>", i + 2);
            if (closePos == std::wstring::npos) {
                emit(i, n, SyntaxKind::Comment);
                inBlockComment = true;
                break;
            }
            emit(i, closePos + 2, SyntaxKind::Comment);
            i = closePos + 2;
            continue;
        }
        if ((lang == SyntaxLang::Python || lang == SyntaxLang::PowerShell || lang == SyntaxLang::Shell ||
             lang == SyntaxLang::CMake || lang == SyntaxLang::Config) &&
            ch == L'#') {
            emit(i, n, SyntaxKind::Comment);
            break;
        }
        if (lang == SyntaxLang::Config && ch == L';' && i == line.find_first_not_of(L' ')) {
            emit(i, n, SyntaxKind::Comment);
            break;
        }
        if (lang == SyntaxLang::Config && ch == L'[' && i == line.find_first_not_of(L' ')) {
            const size_t closeBracket = line.find(L']', i + 1);
            if (closeBracket != std::wstring::npos) {
                emit(i, closeBracket + 1, SyntaxKind::Type);
                i = closeBracket + 1;
                continue;
            }
        }

        if (ch == L'"' || (ch == L'\'' && lang != SyntaxLang::Markdown) ||
            (ch == L'`' && lang == SyntaxLang::Script)) {
            const wchar_t quote = ch;
            const wchar_t escapeCh = (lang == SyntaxLang::PowerShell) ? L'`' : L'\\';
            size_t j = i + 1;
            while (j < n) {
                if (line[j] == escapeCh && j + 1 < n) {
                    j += 2;
                    continue;
                }
                if (line[j] == quote) {
                    ++j;
                    break;
                }
                ++j;
            }
            SyntaxKind strKind = SyntaxKind::String;
            if (lang == SyntaxLang::Json && quote == L'"') {
                size_t k = j;
                while (k < n && line[k] == L' ')
                    ++k;
                if (k < n && line[k] == L':')
                    strKind = SyntaxKind::Variable;
            }
            emit(i, j, strKind);
            i = j;
            continue;
        }

        if ((lang == SyntaxLang::PowerShell || lang == SyntaxLang::Shell || lang == SyntaxLang::CMake) &&
            ch == L'$') {
            size_t j = i + 1;
            if (j < n && line[j] == L'{') {
                const size_t closeBrace = line.find(L'}', j + 1);
                j = (closeBrace == std::wstring::npos) ? n : (closeBrace + 1);
            } else {
                while (j < n && (IsIdentBody(line[j]) || line[j] == L':'))
                    ++j;
            }
            emit(i, std::max(i + 1, j), SyntaxKind::Variable);
            i = std::max(i + 1, j);
            continue;
        }

        if ((ch >= L'0' && ch <= L'9') && (i == 0 || !IsIdentBody(line[i - 1]))) {
            size_t j = i;
            if (ch == L'0' && j + 1 < n && (line[j + 1] == L'x' || line[j + 1] == L'X' ||
                                           line[j + 1] == L'b' || line[j + 1] == L'B')) {
                j += 2;
                while (j < n && (std::iswxdigit(line[j]) || line[j] == L'\'' || line[j] == L'_'))
                    ++j;
            } else {
                while (j < n && ((line[j] >= L'0' && line[j] <= L'9') || line[j] == L'.' ||
                                 line[j] == L'\'' || line[j] == L'_'))
                    ++j;
                if (j < n && (line[j] == L'e' || line[j] == L'E')) {
                    ++j;
                    if (j < n && (line[j] == L'+' || line[j] == L'-'))
                        ++j;
                    while (j < n && line[j] >= L'0' && line[j] <= L'9')
                        ++j;
                }
            }
            while (j < n && (line[j] == L'u' || line[j] == L'U' || line[j] == L'l' || line[j] == L'L' ||
                             line[j] == L'f' || line[j] == L'F'))
                ++j;
            emit(i, j, SyntaxKind::Number);
            i = j;
            continue;
        }

        if (IsIdentStart(ch)) {
            size_t j = i + 1;
            while (j < n && (IsIdentBody(line[j]) || (lang == SyntaxLang::PowerShell && line[j] == L'-')))
                ++j;
            size_t look = j;
            while (look < n && line[look] == L' ')
                ++look;
            const bool hasParen = (look < n && line[look] == L'(');
            const std::wstring_view word(line.data() + i, j - i);
            SyntaxKind kind = ClassifyIdentifier(word, lang, hasParen);
            if (lang == SyntaxLang::Config && i == line.find_first_not_of(L' ')) {
                if (look < n && (line[look] == L'=' || line[look] == L':'))
                    kind = SyntaxKind::Variable;
            }
            emit(i, j, kind);
            i = j;
            continue;
        }

        if (std::wstring_view(L"{}()[].,;:=+-*/<>!&|^~%?@").find(ch) != std::wstring_view::npos) {
            emit(i, i + 1, SyntaxKind::Operator);
            ++i;
            continue;
        }

        size_t j = i + 1;
        while (j < n && line[j] == ch)
            ++j;
        emit(i, j, SyntaxKind::Plain);
        i = j;
    }

    return spans;
}

} // namespace

AiChangesPane::~AiChangesPane() {
    DestroyFonts();
    if (surfaceBrush_) {
        DeleteObject(surfaceBrush_);
        surfaceBrush_ = nullptr;
    }
    if (card1Brush_) {
        DeleteObject(card1Brush_);
        card1Brush_ = nullptr;
    }
    if (fieldBrush_) {
        DeleteObject(fieldBrush_);
        fieldBrush_ = nullptr;
    }
}

bool AiChangesPane::Create(HWND parent, HFONT font, bool dark, int dpi) {
    if (!parent)
        return false;
    uiFont_ = font;
    dark_ = dark;
    dpi_ = dpi > 0 ? dpi : 96;
    RebuildFonts();

    fieldStyle_.dark = &dark_;
    fieldStyle_.font = smallFont_ ? smallFont_ : uiFont_;
    fieldStyle_.dpi = dpi_;
    fieldStyle_.ai = true;

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));

    WNDCLASSEXW hostWc{sizeof(hostWc)};
    hostWc.lpfnWndProc = HostProc;
    hostWc.hInstance = instance;
    hostWc.lpszClassName = kChangesHostClass;
    hostWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&hostWc);

    WNDCLASSEXW viewWc{sizeof(viewWc)};
    viewWc.style = CS_DBLCLKS;
    viewWc.lpfnWndProc = ViewerProc;
    viewWc.hInstance = instance;
    viewWc.lpszClassName = kChangesViewerClass;
    viewWc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    RegisterClassExW(&viewWc);

    surfaceBrush_ = CreateSolidBrush(TrayColor());
    card1Brush_ = CreateSolidBrush(Card1FillColor());
    fieldBrush_ = CreateSolidBrush(UiAiTheme(dark_).field);

    window_ = CreateWindowExW(0, kChangesHostClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 100, 100,
                              parent, nullptr, instance, this);
    if (!window_)
        return false;

    modeCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                     CBS_HASSTRINGS | WS_VSCROLL,
                                 0, 0, 80, 220, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ModeComboBox)),
                                 instance, nullptr);
    if (modeCombo_) {
        SendMessageW(modeCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        UiStyleField(modeCombo_, &fieldStyle_);
        SetPropW(modeCombo_, L"SerialCtl.CompactLabel", reinterpret_cast<HANDLE>(&compactScopeLabel_));
        UiSetControlTooltip(modeCombo_, L"选择本轮、全部、Git 状态或 Git 历史");
    }

    fileCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                     CBS_HASSTRINGS | WS_VSCROLL,
                                 0, 0, 120, 220, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(FileComboBox)),
                                 instance, nullptr);
    if (fileCombo_) {
        SendMessageW(fileCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        UiStyleField(fileCombo_, &fieldStyle_);
        UiSetControlTooltip(fileCombo_, L"切换查看本次修改的文件");
    }

    searchCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL |
                                       CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                   0, 0, 100, 220, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SearchComboBox)),
                                   instance, nullptr);
    if (searchCombo_) {
        SendMessageW(searchCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        UiStyleField(searchCombo_, &fieldStyle_);
        UiSetControlTooltip(searchCombo_, L"搜索修改文本（回车跳转下一处，下拉选择历史关键词）");
        COMBOBOXINFO comboInfo{sizeof(comboInfo)};
        if (GetComboBoxInfo(searchCombo_, &comboInfo) && comboInfo.hwndItem) {
            searchEdit_ = comboInfo.hwndItem;
            SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
            SendMessageW(searchEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L""));
            SetWindowSubclass(searchEdit_, SearchEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    }

    prevMatchButton_ = CreateWindowExW(0, L"BUTTON", L"上一个",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 28, 28,
                                       window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PrevMatchButton)), instance,
                                       nullptr);
    if (prevMatchButton_) {
        SendMessageW(prevMatchButton_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_ ? uiFont_ : smallFont_), TRUE);
        UiStyleButton(prevMatchButton_);
        UiSetFlatIconButton(prevMatchButton_);
        UiSetButtonGlyph(prevMatchButton_, UiGlyph::ChevronUp, L"上一个匹配结果 (Shift+Enter / Shift+F3)");
    }

    nextMatchButton_ = CreateWindowExW(0, L"BUTTON", L"下一个",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 28, 28,
                                       window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(NextMatchButton)), instance,
                                       nullptr);
    if (nextMatchButton_) {
        SendMessageW(nextMatchButton_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_ ? uiFont_ : smallFont_), TRUE);
        UiStyleButton(nextMatchButton_);
        UiSetFlatIconButton(nextMatchButton_);
        UiSetButtonGlyph(nextMatchButton_, UiGlyph::ChevronDown, L"下一个匹配结果 (Enter / F3)");
    }

    viewer_ = CreateWindowExW(0, kChangesViewerClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 100,
                              window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ContentView)), instance, this);

    ApplyChildTheme();
    UpdateButtons();
    UpdateSearchControls();
    RebuildFilteredList();
    Layout();
    return true;
}

COLORREF AiChangesPane::TrayColor() const {
    const auto c = UiTheme(dark_);
    return dark_ ? c.field : c.window;
}

COLORREF AiChangesPane::Card1FillColor() const {
    const auto c = UiTheme(dark_);
    return dark_ ? c.surface : c.surface;
}

int AiChangesPane::HeaderHeightPx() const {
    return SelectedRecord() ? D(30) : 0;
}

int AiChangesPane::GutterWidthPx() const {
    const int digits = std::max(3, static_cast<int>(std::to_string(std::max(1, maxLineNumber_)).size()));
    return digits * std::max(6, viewerCharWidth_) + D(14);
}

void AiChangesPane::ApplyChildTheme() {
    if (surfaceBrush_)
        DeleteObject(surfaceBrush_);
    surfaceBrush_ = CreateSolidBrush(TrayColor());
    if (card1Brush_)
        DeleteObject(card1Brush_);
    card1Brush_ = CreateSolidBrush(Card1FillColor());
    if (fieldBrush_)
        DeleteObject(fieldBrush_);
    fieldBrush_ = CreateSolidBrush(UiAiTheme(dark_).field);

    const COLORREF card1Bg = Card1FillColor();
    UiSetControlBackdrop(modeCombo_, card1Bg);
    UiSetControlBackdrop(fileCombo_, card1Bg);
    UiSetControlBackdrop(searchCombo_, card1Bg);
    UiSetControlBackdrop(searchEdit_, card1Bg);
}

void AiChangesPane::Theme(bool dark, int dpi, HFONT font) {
    dark_ = dark;
    dpi_ = dpi > 0 ? dpi : 96;
    if (font)
        uiFont_ = font;
    RebuildFonts();
    fieldStyle_.font = smallFont_ ? smallFont_ : uiFont_;
    fieldStyle_.dpi = dpi_;
    ApplyChildTheme();

    for (HWND h : {modeCombo_, fileCombo_, searchCombo_, searchEdit_}) {
        if (h)
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
    for (HWND b : {prevMatchButton_, nextMatchButton_}) {
        if (b)
            SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_ ? uiFont_ : smallFont_), TRUE);
    }

    Layout();
    RebuildDisplayRows();
    if (window_)
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void AiChangesPane::SetWrap(bool wrap) {
    if (wrap_ == wrap)
        return;
    wrap_ = wrap;
    RebuildDisplayRows();
    if (viewer_)
        InvalidateRect(viewer_, nullptr, FALSE);
}

void AiChangesPane::SetCopyCallback(std::function<void(const std::wstring &text, const wchar_t *notice)> callback) {
    copyCallback_ = std::move(callback);
}

void AiChangesPane::SetChanges(const std::vector<ai::FileChangeRecord> &records, std::uint64_t currentTurnSeq,
                               const std::string &selectChangeId) {
    records_ = records;
    currentTurnSeq_ = currentTurnSeq;
    if (IsGitScope()) {
        UpdateButtons();
        return;
    }
    UpdateButtons();
    RebuildFilteredList(selectChangeId);
    Layout();
    if (window_)
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void AiChangesPane::SetCurrentTurnSeq(std::uint64_t currentTurnSeq) {
    if (currentTurnSeq_ == currentTurnSeq)
        return;
    currentTurnSeq_ = currentTurnSeq;
    UpdateButtons();
    if (IsGitScope())
        return;
    RebuildFilteredList();
    Layout();
    if (window_)
        InvalidateRect(window_, nullptr, FALSE);
}

void AiChangesPane::FocusChange(const std::string &changeId) {
    if (changeId.empty())
        return;
    bool inCurrentTurn = false;
    bool found = false;
    for (const auto &rec : records_) {
        if (rec.changeId == changeId) {
            found = true;
            inCurrentTurn = (currentTurnSeq_ == 0 || rec.turnSeq == currentTurnSeq_);
            break;
        }
    }
    if (!found)
        return;
    if (IsGitScope())
        SetScope(inCurrentTurn ? Scope::CurrentTurn : Scope::All);
    if (scope_ == Scope::CurrentTurn && !inCurrentTurn)
        scope_ = Scope::All;
    UpdateButtons();
    RebuildFilteredList(changeId);
    Layout();
    if (window_)
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void AiChangesPane::SetScope(Scope scope) {
    if (scope_ == scope)
        return;
    scope_ = scope;
    gitChanges_ = {};
    gitRoot_.clear();
    nextGitRefresh_ = 0;
    if (IsGitScope()) {
        SetTimer(window_, 1, 250, nullptr);
        RefreshGitChanges();
    }
    UpdateButtons();
    RebuildFilteredList();
    Layout();
    if (window_)
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void AiChangesPane::SetGitWorkspace(std::shared_ptr<ai::LocalWorkspaceService> workspace) {
    gitWorkspace_ = std::move(workspace);
    UpdateButtons();
}

bool AiChangesPane::IsGitScope() const {
    return scope_ == Scope::GitStatus || scope_ == Scope::GitHistory;
}

const std::vector<ai::FileChangeRecord> &AiChangesPane::VisibleRecords() const {
    return IsGitScope() ? gitChanges_.records : records_;
}

void AiChangesPane::RefreshGitChanges() {
    if (!IsGitScope() || !gitWorkspace_) {
        KillTimer(window_, 1);
        return;
    }
    const auto workspace = gitWorkspace_->Snapshot();
    const auto &root = workspace.canonicalRoot;
    if (gitRoot_ != root || (gitChanges_.notice.empty() && gitChanges_.records.empty() && !gitQuery_.valid())) {
        gitRoot_ = root;
        gitChanges_ = {{}, workspace.active ? u8"正在读取 Git 数据…" : u8"请先选择本地工作区", {}};
        nextGitRefresh_ = 0;
        RebuildFilteredList();
    }
    if (SendMessageW(fileCombo_, CB_GETDROPPEDSTATE, 0, 0) ||
        SendMessageW(modeCombo_, CB_GETDROPPEDSTATE, 0, 0))
        return;
    if (gitQuery_.valid()) {
        if (gitQuery_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        ai::GitFileChanges result;
        try {
            result = gitQuery_.get();
        } catch (const std::exception &) {
            result.notice = u8"无法读取 Git 数据";
        }
        if (gitQueryScope_ == scope_ && gitQueryRoot_ == root) {
            const bool same = result.notice == gitChanges_.notice &&
                result.previewId == gitChanges_.previewId &&
                std::equal(result.records.begin(), result.records.end(), gitChanges_.records.begin(),
                    gitChanges_.records.end(), [](const auto &a, const auto &b) {
                        return a.changeId == b.changeId && a.detail == b.detail && a.diffText == b.diffText;
                    });
            if (!same) {
                gitChanges_ = std::move(result);
                RebuildFilteredList();
                Layout();
                RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            }
            nextGitRefresh_ = !selectedChangeId_.empty() && selectedChangeId_ != gitChanges_.previewId
                ? 0 : GetTickCount64() + 5000;
        }
    }
    if (!workspace.active || !IsWindowVisible(window_) || GetTickCount64() < nextGitRefresh_)
        return;
    gitQueryRoot_ = root;
    gitQueryScope_ = scope_;
    try {
        std::packaged_task<ai::GitFileChanges()> query([service = gitWorkspace_, workspace,
                                                      history = scope_ == Scope::GitHistory,
                                                      selected = selectedChangeId_] {
            return service->ReadGitFileChanges(workspace, history, selected);
        });
        auto result = query.get_future();
        std::thread(std::move(query)).detach();
        gitQuery_ = std::move(result);
    } catch (const std::exception &) {
        gitChanges_.notice = u8"无法启动 Git 查询";
        nextGitRefresh_ = GetTickCount64() + 5000;
        RebuildFilteredList();
    }
}

void AiChangesPane::SetViewMode(ViewMode mode) {
    if (viewMode_ == mode)
        return;
    viewMode_ = mode;
    viewerScrollY_ = 0;
    viewerScrollX_ = 0;
    hasSelection_ = false;
    UpdateButtons();
    RebuildLogicalLines();
    RebuildDisplayRows();
    RebuildSearchMatches(false);
    if (viewer_)
        InvalidateRect(viewer_, nullptr, FALSE);
}

void AiChangesPane::SetSearchQuery(const std::wstring &query) {
    if (searchQuery_ == query)
        return;
    searchQuery_ = query;
    if (searchCombo_) {
        const std::wstring current = ControlCaption(searchCombo_);
        if (current != searchQuery_) {
            suppressSearchNotify_ = true;
            SetWindowTextW(searchCombo_, searchQuery_.c_str());
            suppressSearchNotify_ = false;
        }
    } else if (searchEdit_) {
        const std::wstring current = ControlCaption(searchEdit_);
        if (current != searchQuery_) {
            suppressSearchNotify_ = true;
            SetWindowTextW(searchEdit_, searchQuery_.c_str());
            suppressSearchNotify_ = false;
        }
    }
    RebuildSearchMatches(true);
    if (searchEdit_)
        InvalidateRect(searchEdit_, nullptr, FALSE);
    if (viewer_)
        InvalidateRect(viewer_, nullptr, FALSE);
}

void AiChangesPane::StepSearchMatch(int delta) {
    if (searchMatches_.empty())
        return;
    const size_t n = searchMatches_.size();
    if (delta >= 0)
        activeMatchIndex_ = (activeMatchIndex_ + 1) % n;
    else
        activeMatchIndex_ = (activeMatchIndex_ + n - 1) % n;

    const auto match = searchMatches_[activeMatchIndex_];
    if (match.inCollapsed && match.collapsedIndex != SIZE_MAX) {
        ExpandCollapsedBlock(match.collapsedIndex);
    }
    if (activeMatchIndex_ < searchMatches_.size()) {
        EnsureViewerRowVisible(searchMatches_[activeMatchIndex_].row);
    }
    UpdateSearchControls();
    if (viewer_)
        InvalidateRect(viewer_, nullptr, FALSE);
}

bool AiChangesPane::ExpandCollapsedBlock(size_t blockIndex) {
    if (blockIndex >= collapsedSections_.size() || collapsedSections_[blockIndex].expanded)
        return false;
    collapsedSections_[blockIndex].expanded = true;
    RebuildDisplayRows();
    RebuildSearchMatches(false);
    if (viewer_)
        InvalidateRect(viewer_, nullptr, FALSE);
    return true;
}

size_t AiChangesPane::CurrentTurnCount() const {
    if (currentTurnSeq_ == 0)
        return records_.size();
    size_t count = 0;
    for (const auto &rec : records_) {
        if (rec.turnSeq == currentTurnSeq_)
            ++count;
    }
    return count;
}

void AiChangesPane::RebuildFonts() {
    DestroyFonts();
    LOGFONTW lf{};
    if (uiFont_ && GetObjectW(uiFont_, sizeof(lf), &lf) > 0) {
        lf.lfHeight = -D(13);
        lf.lfWeight = FW_NORMAL;
        lf.lfItalic = FALSE;
        smallFont_ = CreateFontIndirectW(&lf);
        lf.lfWeight = FW_SEMIBOLD;
        boldSmallFont_ = CreateFontIndirectW(&lf);
    } else {
        ownedUiFont_ = CreateFontW(-D(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI");
        smallFont_ = CreateFontW(-D(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 L"Segoe UI");
        boldSmallFont_ = CreateFontW(-D(13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                     L"Segoe UI");
    }
    codeFont_ = CreateFontW(-D(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    viewerRowHeight_ = std::max(D(20), 14);
    viewerCharWidth_ = std::max(6, D(8));
    if (window_ && codeFont_) {
        HDC dc = GetDC(window_);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, codeFont_);
            TEXTMETRICW tm{};
            if (GetTextMetricsW(dc, &tm)) {
                viewerRowHeight_ = std::max(D(20), static_cast<int>(tm.tmHeight + D(5)));
                viewerCharWidth_ = std::max(4, static_cast<int>(tm.tmAveCharWidth));
            }
            if (old)
                SelectObject(dc, old);
            ReleaseDC(window_, dc);
        }
    }
}

void AiChangesPane::DestroyFonts() {
    auto killFont = [](HFONT &f) {
        if (f) {
            DeleteObject(f);
            f = nullptr;
        }
    };
    killFont(ownedUiFont_);
    killFont(smallFont_);
    killFont(boldSmallFont_);
    killFont(codeFont_);
}

void AiChangesPane::SyncModeComboSelection() {
    if (!modeCombo_)
        return;
    const int idx = static_cast<int>(scope_);
    if (SendMessageW(modeCombo_, CB_GETCURSEL, 0, 0) != idx) {
        SendMessageW(modeCombo_, CB_SETCURSEL, idx, 0);
        InvalidateRect(modeCombo_, nullptr, FALSE);
    }
}

void AiChangesPane::CommitSearchHistory(const std::wstring &query) {
    if (!searchCombo_ || query.empty())
        return;
    const auto it = std::find(searchHistory_.begin(), searchHistory_.end(), query);
    if (it != searchHistory_.end()) {
        if (it == searchHistory_.begin())
            return;
        searchHistory_.erase(it);
    }
    searchHistory_.insert(searchHistory_.begin(), query);
    if (searchHistory_.size() > 12)
        searchHistory_.resize(12);

    const std::wstring currentText = ControlCaption(searchCombo_);
    suppressSearchNotify_ = true;
    SendMessageW(searchCombo_, CB_RESETCONTENT, 0, 0);
    for (const auto &item : searchHistory_)
        SendMessageW(searchCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    SetWindowTextW(searchCombo_, currentText.c_str());
    suppressSearchNotify_ = false;
}

void AiChangesPane::UpdateButtons() {
    if (modeCombo_) {
        compactScopeLabel_ = scope_ == Scope::GitStatus ? L"Git 状态"
                           : scope_ == Scope::GitHistory ? L"Git 历史" : L"DIFF";
        const size_t turnCount = CurrentTurnCount();
        const size_t allCount = records_.size();
        const std::wstring items[] = {
            L"本轮 (" + std::to_wstring(turnCount) + L")",
            L"全部 (" + std::to_wstring(allCount) + L")",
            L"Git 状态",
            L"Git 历史",
        };
        const int targetIdx = static_cast<int>(scope_);
        SendMessageW(modeCombo_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(modeCombo_, CB_RESETCONTENT, 0, 0);
        const int itemCount = gitWorkspace_ ? 4 : 2;
        for (int i = 0; i < itemCount; ++i)
            SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i].c_str()));
        SendMessageW(modeCombo_, CB_SETCURSEL, targetIdx, 0);
        SendMessageW(modeCombo_, CB_SETDROPPEDWIDTH, D(116), 0);
        SendMessageW(modeCombo_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(modeCombo_, nullptr, FALSE);
    }
}

void AiChangesPane::UpdateSearchControls() {
    const BOOL hasMatches = !searchMatches_.empty() ? TRUE : FALSE;
    for (HWND b : {prevMatchButton_, nextMatchButton_}) {
        if (b) {
            if (IsWindowEnabled(b) != hasMatches)
                EnableWindow(b, hasMatches);
            InvalidateRect(b, nullptr, FALSE);
        }
    }
    if (viewer_)
        InvalidateRect(viewer_, nullptr, FALSE);
}

void AiChangesPane::RebuildFilteredList(const std::string &preferSelectId) {
    const auto &records = VisibleRecords();
    filteredIndices_.clear();
    for (size_t i = records.size(); i > 0; --i) {
        const size_t idx = i - 1;
        const auto &rec = records[idx];
        if (scope_ == Scope::CurrentTurn && currentTurnSeq_ > 0 && rec.turnSeq != currentTurnSeq_)
            continue;
        filteredIndices_.push_back(idx);
    }

    std::string target = preferSelectId.empty() ? selectedChangeId_ : preferSelectId;
    bool targetPresent = false;
    for (size_t idx : filteredIndices_) {
        if (records[idx].changeId == target) {
            targetPresent = true;
            break;
        }
    }
    if (!targetPresent)
        target = filteredIndices_.empty() ? std::string() : records[filteredIndices_.front()].changeId;

    const bool selectionChanged = (target != selectedChangeId_);
    selectedChangeId_ = target;

    if (fileCombo_) {
        const auto notice = IsGitScope() ? ToWideText(gitChanges_.notice) : std::wstring{};
        UiSetControlTooltip(fileCombo_, notice.empty() ? L"切换查看修改的文件" : notice.c_str());
        SendMessageW(fileCombo_, CB_RESETCONTENT, 0, 0);
        if (filteredIndices_.empty()) {
            const wchar_t *emptyItem = !notice.empty() ? notice.c_str() : (scope_ == Scope::CurrentTurn && !records.empty())
                                           ? L"本轮暂无文件修改（可切回「全部」）"
                                           : L"暂无修改的文件";
            SendMessageW(fileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(emptyItem));
            SendMessageW(fileCombo_, CB_SETCURSEL, 0, 0);
            EnableWindow(fileCombo_, FALSE);
        } else {
            int selIndex = 0;
            for (size_t i = 0; i < filteredIndices_.size(); ++i) {
                const auto &rec = records[filteredIndices_[i]];
                if (rec.changeId == selectedChangeId_)
                    selIndex = static_cast<int>(i);
                std::string fileName, dirName;
                SplitDisplayPath(rec.path, fileName, dirName);
                std::wstring item = ShortActionTag(rec.action) + L" " + ToWideText(fileName);
                if (rec.addedLines > 0 || rec.removedLines > 0) {
                    item += L" (+" + std::to_wstring(rec.addedLines) + L" / -" + std::to_wstring(rec.removedLines) + L")";
                }
                if (!dirName.empty())
                    item += L" · " + ToWideText(dirName);
                if (scope_ == Scope::GitHistory)
                    item += L" · " + ToWideText(rec.detail.substr(0, 8));
                else if (scope_ == Scope::GitStatus) {
                    using Layer = ai::FileChangeRecord::GitLayer;
                    item += rec.gitLayer == Layer::Staged ? L" · 已暂存"
                          : rec.gitLayer == Layer::Unstaged ? L" · 未暂存" : L" · 未跟踪";
                }
                SendMessageW(fileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
            }
            SendMessageW(fileCombo_, CB_SETCURSEL, selIndex, 0);
            EnableWindow(fileCombo_, TRUE);
        }
        InvalidateRect(fileCombo_, nullptr, FALSE);
    }

    if (selectionChanged) {
        viewerScrollY_ = 0;
        viewerScrollX_ = 0;
        hasSelection_ = false;
    }

    UpdateButtons();
    RebuildLogicalLines();
    RebuildDisplayRows();
    RebuildSearchMatches(!searchQuery_.empty());
}

const ai::FileChangeRecord *AiChangesPane::SelectedRecord() const {
    if (selectedChangeId_.empty())
        return nullptr;
    const auto &records = VisibleRecords();
    for (size_t idx : filteredIndices_) {
        if (records[idx].changeId == selectedChangeId_)
            return &records[idx];
    }
    return nullptr;
}

AiChangesPane::DiffLineKind AiChangesPane::ClassifyLine(const std::string &line, bool diffMode) {
    if (!diffMode)
        return DiffLineKind::Normal;
    if (line.rfind("@@", 0) == 0)
        return DiffLineKind::Hunk;
    if (line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0 || line.rfind("diff --", 0) == 0 ||
        line.rfind("index ", 0) == 0)
        return DiffLineKind::Header;
    if (!line.empty() && line[0] == '+')
        return DiffLineKind::Added;
    if (!line.empty() && line[0] == '-')
        return DiffLineKind::Removed;
    return DiffLineKind::Normal;
}

std::wstring AiChangesPane::ToWideText(const std::string &utf8) {
    if (utf8.empty())
        return {};
    auto wide = MultiByteToWide(reinterpret_cast<const std::uint8_t *>(utf8.data()), utf8.size(), CP_UTF8);
    if (wide.empty())
        wide = MultiByteToWide(reinterpret_cast<const std::uint8_t *>(utf8.data()), utf8.size(), CP_ACP);
    return wide;
}

void AiChangesPane::RebuildLogicalLines() {
    logicalLines_.clear();
    collapsedSections_.clear();
    maxLineNumber_ = 1;

    const auto *rec = SelectedRecord();
    if (!rec)
        return;

    auto pushSectionOrLines = [&](std::vector<LogicalLine> &&segment, bool allowFold, int keepHead, int keepTail) {
        constexpr size_t kMinFoldLines = 5;
        if (!allowFold || segment.size() < static_cast<size_t>(keepHead + keepTail + kMinFoldLines)) {
            for (auto &ln : segment)
                logicalLines_.push_back(std::move(ln));
            return;
        }
        for (int i = 0; i < keepHead; ++i)
            logicalLines_.push_back(std::move(segment[static_cast<size_t>(i)]));

        CollapsedSection sec;
        const size_t start = static_cast<size_t>(keepHead);
        const size_t end = segment.size() - static_cast<size_t>(keepTail);
        sec.lines.reserve(end - start);
        for (size_t i = start; i < end; ++i)
            sec.lines.push_back(std::move(segment[i]));
        const size_t hidden = sec.lines.size();
        const size_t blockId = collapsedSections_.size();
        collapsedSections_.push_back(std::move(sec));

        LogicalLine marker;
        marker.kind = DiffLineKind::Collapsed;
        marker.collapsedIndex = blockId;
        marker.hiddenCount = hidden;
        marker.text = L"+" + std::to_wstring(hidden) + L" more lines";
        logicalLines_.push_back(std::move(marker));

        for (size_t i = end; i < segment.size(); ++i)
            logicalLines_.push_back(std::move(segment[i]));
    };

    auto updateSyntaxBlockState = [&]() {
        const SyntaxLang lang = DetectSyntaxLanguage(rec->path);
        bool inBlock = false;
        auto advanceLine = [&](LogicalLine &ln) {
            if (ln.kind == DiffLineKind::Collapsed || ln.kind == DiffLineKind::Header || ln.kind == DiffLineKind::Hunk)
                return;
            if (ln.kind == DiffLineKind::Removed) {
                ln.startsInBlockComment = inBlock;
                return;
            }
            ln.startsInBlockComment = inBlock;
            bool nextBlock = inBlock;
            TokenizeSyntaxLine(ExpandTabs4(ln.text), lang, nextBlock);
            inBlock = nextBlock;
        };
        for (auto &ln : logicalLines_) {
            if (ln.kind == DiffLineKind::Collapsed && ln.collapsedIndex != SIZE_MAX &&
                ln.collapsedIndex < collapsedSections_.size()) {
                for (auto &inner : collapsedSections_[ln.collapsedIndex].lines)
                    advanceLine(inner);
            } else {
                advanceLine(ln);
            }
        }
    };

    if (viewMode_ == ViewMode::Content) {
        std::string raw = !rec->contentPreview.empty() ? rec->contentPreview : (!rec->diffText.empty() ? rec->diffText : rec->detail);
        const auto lines = SplitUtf8LinesWide(raw, [](const std::string &s) { return ToWideText(s); });
        logicalLines_.reserve(lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            LogicalLine ln;
            ln.kind = DiffLineKind::Normal;
            ln.newLine = static_cast<int>(i + 1);
            ln.oldLine = ln.newLine;
            ln.text = lines[i];
            maxLineNumber_ = std::max(maxLineNumber_, ln.newLine);
            logicalLines_.push_back(std::move(ln));
        }
        updateSyntaxBlockState();
        return;
    }

    std::string rawDiff = !rec->diffText.empty() ? rec->diffText : (!rec->contentPreview.empty() ? rec->contentPreview : rec->detail);
    if (IsGitScope() && rec->changeId != gitChanges_.previewId)
        rawDiff += u8"\n正在读取 Git 补丁…";
    if (rec->diffText.empty()) {
        const auto lines = SplitUtf8LinesWide(rawDiff, [](const std::string &s) { return ToWideText(s); });
        const DiffLineKind defaultKind = IsGitScope() ? DiffLineKind::Normal
                                       : rec->action == "create" ? DiffLineKind::Added
                                       : rec->action == "delete" ? DiffLineKind::Removed
                                                                 : DiffLineKind::Normal;
        std::vector<LogicalLine> segment;
        segment.reserve(lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            LogicalLine ln;
            ln.kind = defaultKind;
            ln.newLine = static_cast<int>(i + 1);
            ln.oldLine = ln.newLine;
            ln.text = lines[i];
            maxLineNumber_ = std::max(maxLineNumber_, ln.newLine);
            segment.push_back(std::move(ln));
        }
        pushSectionOrLines(std::move(segment), defaultKind == DiffLineKind::Normal, 7, 3);
        updateSyntaxBlockState();
        return;
    }

    std::vector<std::wstring> previewLines;
    if (!rec->contentPreview.empty())
        previewLines = SplitUtf8LinesWide(rec->contentPreview, [](const std::string &s) { return ToWideText(s); });

    std::vector<std::string> diffLines;
    {
        size_t pos = 0;
        while (pos <= rawDiff.size()) {
            const size_t nl = rawDiff.find('\n', pos);
            std::string line = (nl == std::string::npos) ? rawDiff.substr(pos) : rawDiff.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            diffLines.push_back(std::move(line));
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
            if (pos == rawDiff.size())
                break;
        }
    }

    struct HunkBlock {
        int oldStart = 1;
        int oldCount = 0;
        int newStart = 1;
        int newCount = 0;
        std::vector<std::string> body;
    };
    std::vector<HunkBlock> hunks;
    std::vector<std::string> preamble;
    for (const auto &line : diffLines) {
        int os = 0, oc = 0, ns = 0, nc = 0;
        if (ParseHunkHeader(line, os, oc, ns, nc)) {
            hunks.push_back({os, oc, ns, nc, {}});
        } else if (!hunks.empty()) {
            if (!line.empty() && (line[0] == ' ' || line[0] == '+' || line[0] == '-' || line[0] == '\\'))
                hunks.back().body.push_back(line);
        } else {
            if (line.rfind("--- ", 0) != 0 && line.rfind("+++ ", 0) != 0 && line.rfind("diff --", 0) != 0 &&
                line.rfind("index ", 0) != 0)
                preamble.push_back(line);
        }
    }

    if (hunks.empty()) {
        for (size_t i = 0; i < diffLines.size(); ++i) {
            LogicalLine ln;
            ln.kind = ClassifyLine(diffLines[i], true);
            ln.newLine = static_cast<int>(i + 1);
            ln.text = ToWideText(diffLines[i]);
            maxLineNumber_ = std::max(maxLineNumber_, ln.newLine);
            logicalLines_.push_back(std::move(ln));
        }
        updateSyntaxBlockState();
        return;
    }

    int cursorNewLine = 1;
    int cursorOldLine = 1;
    if (IsGitScope() && rec->truncated) {
        LogicalLine notice;
        notice.kind = DiffLineKind::Header;
        notice.text = L"补丁达到显示上限";
        logicalLines_.push_back(std::move(notice));
    }

    auto appendGap = [&](int targetNewStart, int keepTop, int keepBottom) {
        if (targetNewStart <= cursorNewLine)
            return;
        const int gapCount = targetNewStart - cursorNewLine;
        if (!previewLines.empty() && cursorNewLine >= 1 && targetNewStart - 1 <= static_cast<int>(previewLines.size())) {
            std::vector<LogicalLine> gapSeg;
            gapSeg.reserve(static_cast<size_t>(gapCount));
            for (int n = cursorNewLine; n < targetNewStart; ++n) {
                LogicalLine ln;
                ln.kind = DiffLineKind::Normal;
                ln.newLine = n;
                ln.oldLine = cursorOldLine + (n - cursorNewLine);
                ln.text = previewLines[static_cast<size_t>(n - 1)];
                maxLineNumber_ = std::max(maxLineNumber_, n);
                gapSeg.push_back(std::move(ln));
            }
            pushSectionOrLines(std::move(gapSeg), true, keepTop, keepBottom);
        } else if (gapCount > 0) {
            LogicalLine marker;
            marker.kind = DiffLineKind::Collapsed;
            marker.hiddenCount = static_cast<size_t>(gapCount);
            marker.text = L"+" + std::to_wstring(gapCount) + L" more lines";
            logicalLines_.push_back(std::move(marker));
        }
    };

    for (size_t hi = 0; hi < hunks.size(); ++hi) {
        const auto &hunk = hunks[hi];
        if (hi == 0) {
            if (hunk.newStart > 1)
                appendGap(hunk.newStart, 7, 2);
        } else {
            if (hunk.newStart > cursorNewLine)
                appendGap(hunk.newStart, 2, 2);
        }

        int oldNo = hunk.oldStart;
        int newNo = hunk.newStart;
        std::vector<LogicalLine> contextRun;

        auto flushContextRun = [&]() {
            if (contextRun.empty())
                return;
            pushSectionOrLines(std::move(contextRun), true, 3, 3);
            contextRun.clear();
        };

        for (const auto &raw : hunk.body) {
            if (raw.rfind("\\ No newline", 0) == 0)
                continue;
            const char tag = raw.empty() ? ' ' : raw[0];
            const std::string bodyUtf8 = raw.size() > 1 ? raw.substr(1) : std::string();
            const std::wstring bodyWide = ToWideText(bodyUtf8);

            if (tag == '+') {
                flushContextRun();
                LogicalLine ln;
                ln.kind = DiffLineKind::Added;
                ln.newLine = newNo++;
                ln.text = bodyWide;
                maxLineNumber_ = std::max(maxLineNumber_, ln.newLine);
                logicalLines_.push_back(std::move(ln));
            } else if (tag == '-') {
                flushContextRun();
                LogicalLine ln;
                ln.kind = DiffLineKind::Removed;
                ln.oldLine = oldNo++;
                ln.text = bodyWide;
                maxLineNumber_ = std::max(maxLineNumber_, ln.oldLine);
                logicalLines_.push_back(std::move(ln));
            } else {
                LogicalLine ln;
                ln.kind = DiffLineKind::Normal;
                ln.oldLine = oldNo++;
                ln.newLine = newNo++;
                ln.text = bodyWide;
                maxLineNumber_ = std::max(maxLineNumber_, std::max(ln.oldLine, ln.newLine));
                contextRun.push_back(std::move(ln));
            }
        }
        flushContextRun();
        cursorOldLine = oldNo;
        cursorNewLine = newNo;
    }

    if (!previewLines.empty() && cursorNewLine <= static_cast<int>(previewLines.size())) {
        const int tailTarget = static_cast<int>(previewLines.size()) + 1;
        appendGap(tailTarget, 3, 0);
    }
    updateSyntaxBlockState();
}

void AiChangesPane::RebuildDisplayRows() {
    rows_.clear();
    viewerMaxLinePx_ = 0;

    RECT client{};
    if (viewer_)
        GetClientRect(viewer_, &client);
    const int headerH = HeaderHeightPx();
    const int gutterW = GutterWidthPx();
    const int viewW = std::max(D(80), static_cast<int>(client.right - client.left));
    const int availPx = std::max(D(60), viewW - gutterW - D(14) - D(Ui::OverlayScrollLaneWidth96));

    HDC measureDc = viewer_ ? GetDC(viewer_) : nullptr;
    HGDIOBJ oldFont = (measureDc && codeFont_) ? SelectObject(measureDc, codeFont_) : nullptr;
    auto measureWidth = [&](const std::wstring &s) -> int {
        if (s.empty())
            return 0;
        if (measureDc) {
            SIZE sz{};
            if (GetTextExtentPoint32W(measureDc, s.c_str(), static_cast<int>(s.size()), &sz))
                return sz.cx;
        }
        return static_cast<int>(s.size()) * viewerCharWidth_;
    };

    auto appendLogicalLine = [&](size_t logicalIndex, size_t collapsedId, size_t collapsedInnerIdx,
                                 const LogicalLine &ln) {
        if (ln.kind == DiffLineKind::Collapsed) {
            rows_.push_back({logicalIndex, 0, DiffLineKind::Collapsed, 0, 0, ln.collapsedIndex, SIZE_MAX, false,
                             false, ln.text});
            return;
        }

        std::wstring expanded = ExpandTabs4(ln.text);

        if (!wrap_ || expanded.empty()) {
            viewerMaxLinePx_ = (std::max)(viewerMaxLinePx_, measureWidth(expanded));
            rows_.push_back({logicalIndex, 0, ln.kind, ln.oldLine, ln.newLine, collapsedId, collapsedInnerIdx, false,
                             ln.startsInBlockComment, std::move(expanded)});
            return;
        }

        size_t pos = 0;
        bool cont = false;
        while (pos < expanded.size()) {
            size_t count = expanded.size() - pos;
            if (measureDc) {
                int fit = 0;
                SIZE sz{};
                if (GetTextExtentExPointW(measureDc, expanded.c_str() + pos, static_cast<int>(count), availPx, &fit,
                                          nullptr, &sz) &&
                    fit > 0) {
                    count = static_cast<size_t>(fit);
                } else {
                    count = std::max<size_t>(1, static_cast<size_t>(availPx / std::max(1, viewerCharWidth_)));
                }
            } else {
                const size_t approx = std::max<size_t>(1, static_cast<size_t>(availPx / std::max(1, viewerCharWidth_)));
                count = (std::min)(count, approx);
            }
            if (count == 0)
                count = 1;
            rows_.push_back({logicalIndex, pos, ln.kind, ln.oldLine, ln.newLine, collapsedId, collapsedInnerIdx, cont,
                             ln.startsInBlockComment, expanded.substr(pos, count)});
            pos += count;
            cont = true;
        }
    };

    for (size_t i = 0; i < logicalLines_.size(); ++i) {
        const auto &ln = logicalLines_[i];
        if (ln.kind == DiffLineKind::Collapsed && ln.collapsedIndex != SIZE_MAX &&
            ln.collapsedIndex < collapsedSections_.size() && collapsedSections_[ln.collapsedIndex].expanded) {
            const auto &innerLines = collapsedSections_[ln.collapsedIndex].lines;
            for (size_t j = 0; j < innerLines.size(); ++j)
                appendLogicalLine(i, ln.collapsedIndex, j, innerLines[j]);
        } else {
            appendLogicalLine(i, SIZE_MAX, SIZE_MAX, ln);
        }
    }

    if (oldFont && measureDc)
        SelectObject(measureDc, oldFont);
    if (measureDc && viewer_)
        ReleaseDC(viewer_, measureDc);

    (void)headerH;
    ScrollViewerY(0);
    ScrollViewerX(0);
}

void AiChangesPane::RebuildSearchMatches(bool jumpToFirst) {
    searchMatches_.clear();
    activeMatchIndex_ = 0;
    if (searchQuery_.empty()) {
        UpdateSearchControls();
        return;
    }

    const std::wstring needle = ToLowerWide(searchQuery_);
    for (size_t r = 0; r < rows_.size(); ++r) {
        const auto &row = rows_[r];
        if (row.kind == DiffLineKind::Collapsed) {
            if (row.collapsedIndex != SIZE_MAX && row.collapsedIndex < collapsedSections_.size()) {
                const auto &sec = collapsedSections_[row.collapsedIndex];
                if (!sec.expanded) {
                    for (const auto &hidden : sec.lines) {
                        const std::wstring hay = ToLowerWide(hidden.text);
                        size_t pos = 0;
                        while ((pos = hay.find(needle, pos)) != std::wstring::npos) {
                            searchMatches_.push_back({true, row.collapsedIndex, r, pos, needle.size()});
                            pos += std::max<size_t>(1, needle.size());
                        }
                    }
                }
            }
            continue;
        }
        const std::wstring hay = ToLowerWide(row.text);
        size_t pos = 0;
        while ((pos = hay.find(needle, pos)) != std::wstring::npos) {
            searchMatches_.push_back({false, SIZE_MAX, r, pos, needle.size()});
            pos += std::max<size_t>(1, needle.size());
        }
    }

    if (jumpToFirst && !searchMatches_.empty()) {
        const auto first = searchMatches_.front();
        if (first.inCollapsed && first.collapsedIndex != SIZE_MAX) {
            if (ExpandCollapsedBlock(first.collapsedIndex) && !searchMatches_.empty())
                EnsureViewerRowVisible(searchMatches_.front().row);
            return;
        }
        EnsureViewerRowVisible(first.row);
    }
    UpdateSearchControls();
}

void AiChangesPane::Layout() {
    if (!window_)
        return;
    RECT client{};
    GetClientRect(window_, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));

    const int cardPadX = D(8);
    const int rowY = D(6);
    const int fieldH = Ui::CompactHeight;
    const int gap = D(6);
    const int innerW = std::max(D(120), width - cardPadX * 2);

    const int card1H = rowY + fieldH + D(6);
    card1Rect_ = {0, 0, width, card1H};

    const int viewerTop = card1H + Ui::Space;
    const int viewerH = std::max(D(48), height - viewerTop);

    const int navBtnW = fieldH;
    const int navGap = D(4);
    const int navGroupW = navBtnW * 2 + navGap;
    const int modeW = std::clamp(D(72), D(64), std::max(D(64), innerW / 4));
    const int restBudget = std::max(D(90), innerW - modeW - navGroupW - gap * 3);
    int searchW = std::clamp(MulDiv(restBudget, 45, 100), D(68), D(180));
    int fileW = restBudget - searchW;
    if (fileW < D(72)) {
        const int deficit = D(72) - fileW;
        searchW = std::max(D(54), searchW - deficit);
        fileW = std::max(D(48), restBudget - searchW);
    }

    UiLayoutBatch batch(true);
    int x = cardPadX;
    if (modeCombo_) {
        batch.Move(modeCombo_, x, rowY, modeW, D(220));
        batch.ComboVisibleHeight(modeCombo_, fieldH);
        SendMessageW(modeCombo_, CB_SETDROPPEDWIDTH, D(116), 0);
        x += modeW + gap;
    }
    if (fileCombo_) {
        batch.Move(fileCombo_, x, rowY, fileW, D(220));
        batch.ComboVisibleHeight(fileCombo_, fieldH);
        SendMessageW(fileCombo_, CB_SETDROPPEDWIDTH, std::max(fileW, D(260)), 0);
        x += fileW + gap;
    }
    if (searchCombo_) {
        batch.Move(searchCombo_, x, rowY, searchW, D(220));
        batch.ComboVisibleHeight(searchCombo_, fieldH);
        SendMessageW(searchCombo_, CB_SETDROPPEDWIDTH, std::max(searchW, D(180)), 0);
        x += searchW + gap;
    }
    const int navStartX = cardPadX + innerW - navGroupW;
    if (prevMatchButton_) {
        batch.Move(prevMatchButton_, navStartX, rowY, navBtnW, fieldH);
    }
    if (nextMatchButton_) {
        batch.Move(nextMatchButton_, navStartX + navBtnW + navGap, rowY, navBtnW, fieldH);
    }

    if (viewer_)
        batch.Move(viewer_, 0, viewerTop, width, viewerH);

    batch.Commit();

    if (wrap_)
        RebuildDisplayRows();
    ScrollViewerY(0);
}

void AiChangesPane::PaintHost(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, surfaceBrush_);

    const auto c = UiTheme(dark_);
    if (card1Rect_.right > card1Rect_.left && card1Rect_.bottom > card1Rect_.top) {
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, 0, 0, card1Rect_.right, card1Rect_.bottom);
        RECT extended{0, -D(44), card1Rect_.right, card1Rect_.bottom};
        UiBox(dc, extended, Card1FillColor(), c.border, D(8));
        RestoreDC(dc, saved);
    }
}

RECT AiChangesPane::CollapsedPillRectForRow(int top, int bottom, int gutterW, int clipRight, const std::wstring &text, HDC dc) const {
    int textW = static_cast<int>(text.size()) * viewerCharWidth_;
    if (dc && smallFont_) {
        HGDIOBJ old = SelectObject(dc, smallFont_);
        SIZE sz{};
        if (!text.empty() && GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz))
            textW = sz.cx;
        if (old)
            SelectObject(dc, old);
    }
    const int pillW = std::min(std::max(D(88), textW + D(20)), std::max(D(60), clipRight - gutterW - D(16)));
    const int areaLeft = gutterW + D(8);
    const int cx = (areaLeft + std::max(areaLeft + pillW, clipRight)) / 2;
    const int pillH = std::max(D(16), (bottom - top) - D(3));
    const int py = top + ((bottom - top) - pillH) / 2;
    return {cx - pillW / 2, py, cx + pillW / 2, py + pillH};
}

void AiChangesPane::PaintViewer(HDC target) {
    RECT client{};
    GetClientRect(viewer_, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    if (w <= 0 || h <= 0)
        return;

    HDC dc = viewerBuffer_.Get(target, w, h, dpi_);
    if (!dc)
        dc = target;

    const auto c = UiTheme(dark_);
    const RECT bgRect{0, 0, w, h};
    const int radius = D(8);
    const COLORREF viewerBg = dark_ ? c.window : c.field;

    HBRUSH trayBr = CreateSolidBrush(TrayColor());
    FillRect(dc, &bgRect, trayBr);
    DeleteObject(trayBr);
    UiBox(dc, bgRect, viewerBg, UiPanelBorder(dark_), radius);

    const auto *rec = SelectedRecord();
    if (!rec || rows_.empty()) {
        const std::wstring msg = !rec
                                     ? std::wstring()
                                     : (!rec->detail.empty() ? ToWideText(rec->detail) : L"该操作无文本 Diff 预览");
        if (!msg.empty()) {
            RECT inset = bgRect;
            inset.left += D(12);
            inset.right -= D(12);
            DrawTextBox(dc, msg, inset, smallFont_, c.muted, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        }
        if (dc != target)
            BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
        return;
    }

    const int headerH = HeaderHeightPx();
    const int gutterW = GutterWidthPx();
    const int padY = D(6);
    const int rowH = std::max(D(18), viewerRowHeight_);
    const int bodyH = std::max(1, h - headerH);
    const int totalH = padY * 2 + static_cast<int>(rows_.size()) * rowH;
    const int scrollLane = (totalH > bodyH) ? D(Ui::OverlayScrollLaneWidth96) : 0;
    const int clipRight = std::max(gutterW + D(16), w - D(4) - scrollLane);
    const int codeLeft = gutterW + D(10);

    const int savedCard = SaveDC(dc);
    if (HRGN cardRgn = CreateRoundRectRgn(1, 1, w, h, radius * 2, radius * 2)) {
        SelectClipRgn(dc, cardRgn);
        DeleteObject(cardRgn);
    }

    // Sticky file header strip matching reference design
    if (headerH > 0) {
        const RECT hdrRect{0, 0, w, headerH};
        const COLORREF hdrBg = dark_ ? c.surface : c.raised;
        HBRUSH hdrBr = CreateSolidBrush(hdrBg);
        FillRect(dc, &hdrRect, hdrBr);
        DeleteObject(hdrBr);
        UiSeparator(dc, 0, w, headerH - 1, UiPanelBorder(dark_));

        std::string fileName, dirName;
        SplitDisplayPath(rec->path, fileName, dirName);
        const std::wstring wFile = ToWideText(fileName);
        const std::wstring wDir = ToWideText(dirName);
        const std::wstring statsAdd = (rec->addedLines > 0 || (rec->removedLines == 0 && rec->action == "create"))
                                          ? (L"+" + std::to_wstring(rec->addedLines))
                                          : std::wstring();
        const std::wstring statsRem = (rec->removedLines > 0 || (rec->addedLines == 0 && rec->action == "delete"))
                                          ? (L"-" + std::to_wstring(rec->removedLines))
                                          : std::wstring();

        auto measureText = [&](const std::wstring &s, HFONT f) -> int {
            if (s.empty())
                return 0;
            HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
            SIZE sz{};
            GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
            if (old)
                SelectObject(dc, old);
            return sz.cx;
        };

        int rightCursor = w - D(10);
        if (!searchQuery_.empty()) {
            const std::wstring matchBadge =
                searchMatches_.empty()
                    ? L"0/0"
                    : (std::to_wstring(activeMatchIndex_ + 1) + L"/" + std::to_wstring(searchMatches_.size()));
            const int mw = measureText(matchBadge, boldSmallFont_) + D(4);
            RECT mRect{rightCursor - mw, 0, rightCursor, headerH - 1};
            DrawTextBox(dc, matchBadge, mRect, boldSmallFont_, searchMatches_.empty() ? c.danger : UiAccentInk(dark_),
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            rightCursor -= mw + D(6);
        }
        if (!statsRem.empty()) {
            const int sw = measureText(statsRem, boldSmallFont_) + D(4);
            RECT rRect{rightCursor - sw, 0, rightCursor, headerH - 1};
            DrawTextBox(dc, statsRem, rRect, boldSmallFont_, c.danger, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            rightCursor -= sw + D(4);
        }
        if (!statsAdd.empty()) {
            const int sw = measureText(statsAdd, boldSmallFont_) + D(4);
            const COLORREF addInk = dark_ ? RGB(86, 214, 132) : RGB(22, 128, 61);
            RECT aRect{rightCursor - sw, 0, rightCursor, headerH - 1};
            DrawTextBox(dc, statsAdd, aRect, boldSmallFont_, addInk, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            rightCursor -= sw + D(8);
        }

        const int fileW = std::min(std::max(D(40), rightCursor - D(12)), measureText(wFile, boldSmallFont_) + D(4));
        RECT fRect{D(10), 0, D(10) + fileW, headerH - 1};
        DrawTextBox(dc, wFile, fRect, boldSmallFont_, c.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!wDir.empty() && fRect.right + D(8) < rightCursor) {
            RECT dRect{fRect.right + D(6), 0, rightCursor, headerH - 1};
            DrawTextBox(dc, wDir, dRect, smallFont_, c.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        }
    }

    const int savedBody = SaveDC(dc);
    IntersectClipRect(dc, 1, headerH, w - 1 - scrollLane, h - 1);

    const TextPoint selLo = hasSelection_ ? (selAnchor_ < selFocus_ ? selAnchor_ : selFocus_) : TextPoint{};
    const TextPoint selHi = hasSelection_ ? (selAnchor_ < selFocus_ ? selFocus_ : selAnchor_) : TextPoint{};

    const COLORREF addBg = dark_ ? RGB(22, 54, 34) : RGB(228, 248, 234);
    const COLORREF remBg = dark_ ? RGB(66, 26, 28) : RGB(253, 230, 230);
    const COLORREF addGutterBg = dark_ ? RGB(28, 70, 44) : RGB(204, 240, 214);
    const COLORREF remGutterBg = dark_ ? RGB(82, 32, 34) : RGB(248, 206, 206);
    const COLORREF addInk = dark_ ? RGB(118, 235, 156) : RGB(21, 128, 61);
    const COLORREF remInk = dark_ ? RGB(255, 130, 126) : RGB(185, 28, 28);
    const COLORREF matchBg = dark_ ? RGB(96, 72, 18) : RGB(255, 230, 138);
    const COLORREF activeMatchBg = dark_ ? RGB(196, 108, 24) : RGB(255, 174, 52);

    // Subtle vertical divider separating line-number gutter from code body
    UiSeparator(dc, gutterW, gutterW + 1, headerH, UiPanelBorder(dark_));

    HGDIOBJ oldFont = codeFont_ ? SelectObject(dc, codeFont_) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW codeMetrics{};
    GetTextMetricsW(dc, &codeMetrics);
    const int codeAscentPad = std::max(0, (rowH - static_cast<int>(codeMetrics.tmHeight)) / 2);
    const SyntaxLang syntaxLang = DetectSyntaxLanguage(rec->path);

    const int firstRow = std::max(0, (viewerScrollY_ - padY) / rowH);
    const int lastRow = std::min(static_cast<int>(rows_.size()) - 1, (viewerScrollY_ + bodyH) / rowH + 1);

    const std::wstring needleLower = ToLowerWide(searchQuery_);

    for (int r = firstRow; r <= lastRow; ++r) {
        const int top = headerH + padY + r * rowH - viewerScrollY_;
        const int bottom = top + rowH;
        if (bottom <= headerH || top >= h)
            continue;
        const auto &row = rows_[static_cast<size_t>(r)];

        if (row.kind == DiffLineKind::Collapsed) {
            const int midY = (top + bottom) / 2;
            UiSeparator(dc, D(6), w - D(6) - scrollLane, midY, UiPanelBorder(dark_));
            const RECT pill = CollapsedPillRectForRow(top, bottom, gutterW, clipRight, row.text, dc);
            const COLORREF pillFill = dark_ ? c.raised : c.raised;
            UiBox(dc, pill, pillFill, c.border, UiCapsuleRadius(pill));
            DrawTextBox(dc, row.text, pill, smallFont_, c.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            continue;
        }

        COLORREF lineBg = viewerBg;
        COLORREF gutterBg = viewerBg;
        COLORREF numInk = c.muted;
        COLORREF fallbackInk = SyntaxTokenColor(SyntaxKind::Plain, dark_, c);
        if (row.kind == DiffLineKind::Added) {
            lineBg = addBg;
            gutterBg = addGutterBg;
            numInk = addInk;
        } else if (row.kind == DiffLineKind::Removed) {
            lineBg = remBg;
            gutterBg = remGutterBg;
            numInk = remInk;
        } else if (row.kind == DiffLineKind::Header) {
            fallbackInk = c.muted;
        } else if (row.kind == DiffLineKind::Hunk) {
            fallbackInk = UiAccentInk(dark_);
        }

        if (gutterBg != viewerBg) {
            RECT gb{1, top, gutterW, bottom};
            HBRUSH br = CreateSolidBrush(gutterBg);
            FillRect(dc, &gb, br);
            DeleteObject(br);
        }
        if (lineBg != viewerBg) {
            RECT band{gutterW + 1, top, w - 1 - scrollLane, bottom};
            HBRUSH br = CreateSolidBrush(lineBg);
            FillRect(dc, &band, br);
            DeleteObject(br);
        }

        if (!row.continuation) {
            const int displayNum = (row.newLine > 0) ? row.newLine : row.oldLine;
            if (displayNum > 0) {
                const std::wstring numStr = std::to_wstring(displayNum);
                RECT numRect{D(2), top, gutterW - D(6), bottom};
                SetTextColor(dc, numInk);
                DrawTextW(dc, numStr.c_str(), static_cast<int>(numStr.size()), &numRect,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }

        std::vector<int> extents;
        if (!row.text.empty()) {
            extents.resize(row.text.size(), 0);
            int fit = 0;
            SIZE sz{};
            if (!GetTextExtentExPointW(dc, row.text.c_str(), static_cast<int>(row.text.size()), INT_MAX, &fit,
                                       extents.data(), &sz)) {
                for (size_t i = 0; i < row.text.size(); ++i)
                    extents[i] = static_cast<int>((i + 1) * viewerCharWidth_);
            }
        }

        auto measurePrefix = [&](size_t col) -> int {
            if (col == 0 || extents.empty())
                return 0;
            const size_t clamped = (std::min)(col, extents.size());
            return extents[clamped - 1];
        };

        // Search highlights under text
        if (!needleLower.empty() && !row.text.empty()) {
            const std::wstring hay = ToLowerWide(row.text);
            size_t pos = 0;
            while ((pos = hay.find(needleLower, pos)) != std::wstring::npos) {
                const size_t rIdx = static_cast<size_t>(r);
                bool isActive = false;
                if (activeMatchIndex_ < searchMatches_.size()) {
                    const auto &am = searchMatches_[activeMatchIndex_];
                    isActive = !am.inCollapsed && am.row == rIdx && am.startCol == pos;
                }
                const int hx0 = codeLeft - viewerScrollX_ + measurePrefix(pos);
                const int hx1 = codeLeft - viewerScrollX_ + measurePrefix(pos + needleLower.size());
                RECT hRect{std::max(codeLeft, hx0), top + 1, std::min(clipRight, hx1), bottom - 1};
                if (hRect.right > hRect.left) {
                    HBRUSH hbr = CreateSolidBrush(isActive ? activeMatchBg : matchBg);
                    FillRect(dc, &hRect, hbr);
                    DeleteObject(hbr);
                }
                pos += std::max<size_t>(1, needleLower.size());
            }
        }

        const size_t rowIdx = static_cast<size_t>(r);
        if (hasSelection_ && selLo != selHi && rowIdx >= selLo.row && rowIdx <= selHi.row) {
            const size_t c0 = (rowIdx == selLo.row) ? (std::min)(selLo.col, row.text.size()) : 0;
            const size_t c1 = (rowIdx == selHi.row) ? (std::min)(selHi.col, row.text.size()) : row.text.size();
            const int x0 = codeLeft - viewerScrollX_ + measurePrefix(c0);
            int x1 = codeLeft - viewerScrollX_ + measurePrefix(c1);
            if (rowIdx < selHi.row && c1 == row.text.size())
                x1 = std::max(x1, x0 + viewerCharWidth_);
            if (x1 > x0) {
                RECT selRect{std::max(codeLeft, x0), top, std::min(clipRight, x1), bottom};
                if (selRect.right > selRect.left) {
                    HBRUSH selBrush = CreateSolidBrush(c.accentSoft);
                    FillRect(dc, &selRect, selBrush);
                    DeleteObject(selBrush);
                }
            }
        }

        if (!row.text.empty()) {
            const int savedTextClip = SaveDC(dc);
            IntersectClipRect(dc, codeLeft, std::max(headerH, top), clipRight, std::min(h - 1, bottom));
            const int baseX = codeLeft - viewerScrollX_;
            const int textY = top + codeAscentPad;

            if (row.kind == DiffLineKind::Header || row.kind == DiffLineKind::Hunk || syntaxLang == SyntaxLang::Plain) {
                SetTextColor(dc, fallbackInk);
                ExtTextOutW(dc, baseX, textY, 0, nullptr, row.text.c_str(), static_cast<UINT>(row.text.size()), nullptr);
            } else {
                const bool isWrapped =
                    row.continuation ||
                    (static_cast<size_t>(r + 1) < rows_.size() &&
                     rows_[static_cast<size_t>(r + 1)].logicalIndex == row.logicalIndex &&
                     rows_[static_cast<size_t>(r + 1)].collapsedInnerIndex == row.collapsedInnerIndex &&
                     rows_[static_cast<size_t>(r + 1)].continuation);

                bool inBlock = row.startsInBlockComment;
                std::vector<SyntaxSpan> spans;
                if (!isWrapped) {
                    spans = TokenizeSyntaxLine(row.text, syntaxLang, inBlock);
                } else {
                    std::wstring fullExpanded;
                    if (row.collapsedIndex != SIZE_MAX && row.collapsedIndex < collapsedSections_.size() &&
                        row.collapsedInnerIndex < collapsedSections_[row.collapsedIndex].lines.size()) {
                        fullExpanded = ExpandTabs4(
                            collapsedSections_[row.collapsedIndex].lines[row.collapsedInnerIndex].text);
                    } else if (row.logicalIndex < logicalLines_.size()) {
                        fullExpanded = ExpandTabs4(logicalLines_[row.logicalIndex].text);
                    } else {
                        fullExpanded = row.text;
                    }
                    spans = TokenizeSyntaxLine(fullExpanded, syntaxLang, inBlock);
                }

                const size_t sliceBegin = row.logicalOffset;
                const size_t sliceEnd = sliceBegin + row.text.size();
                size_t drawnUpTo = 0;
                for (const auto &span : spans) {
                    if (span.end <= sliceBegin)
                        continue;
                    if (span.start >= sliceEnd)
                        break;
                    const size_t localStart = (span.start > sliceBegin) ? (span.start - sliceBegin) : 0;
                    const size_t localEnd = std::min(span.end - sliceBegin, row.text.size());
                    if (localStart > drawnUpTo) {
                        SetTextColor(dc, fallbackInk);
                        ExtTextOutW(dc, baseX + measurePrefix(drawnUpTo), textY, 0, nullptr,
                                    row.text.data() + drawnUpTo, static_cast<UINT>(localStart - drawnUpTo), nullptr);
                    }
                    if (localEnd > localStart) {
                        SetTextColor(dc, SyntaxTokenColor(span.kind, dark_, c));
                        ExtTextOutW(dc, baseX + measurePrefix(localStart), textY, 0, nullptr,
                                    row.text.data() + localStart, static_cast<UINT>(localEnd - localStart), nullptr);
                        drawnUpTo = localEnd;
                    }
                }
                if (drawnUpTo < row.text.size()) {
                    SetTextColor(dc, fallbackInk);
                    ExtTextOutW(dc, baseX + measurePrefix(drawnUpTo), textY, 0, nullptr,
                                row.text.data() + drawnUpTo, static_cast<UINT>(row.text.size() - drawnUpTo), nullptr);
                }
            }
            RestoreDC(dc, savedTextClip);
        }
    }

    if (oldFont)
        SelectObject(dc, oldFont);
    RestoreDC(dc, savedBody);
    DrawOverlayScrollbar(dc, ViewerScrollbarThumbRect(client), c, dpi_);
    RestoreDC(dc, savedCard);

    DrawRoundedBorderStroke(dc, bgRect, radius, UiPanelBorder(dark_));

    if (dc != target)
        BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
}

void AiChangesPane::ScrollViewerY(int deltaPx) {
    RECT client{};
    if (viewer_)
        GetClientRect(viewer_, &client);
    const int bodyH = std::max(1, static_cast<int>(client.bottom - client.top) - HeaderHeightPx());
    const int totalH = D(12) + static_cast<int>(rows_.size()) * std::max(D(18), viewerRowHeight_);
    const int maxScroll = std::max(0, totalH - bodyH);
    viewerScrollY_ = std::clamp(viewerScrollY_ + deltaPx, 0, maxScroll);
    if (viewer_)
        InvalidateRect(viewer_, nullptr, FALSE);
}

void AiChangesPane::ScrollViewerX(int deltaPx) {
    if (wrap_) {
        viewerScrollX_ = 0;
        return;
    }
    RECT client{};
    if (viewer_)
        GetClientRect(viewer_, &client);
    const int viewW = std::max(1, static_cast<int>(client.right - client.left) - GutterWidthPx() - D(14) -
                                      D(Ui::OverlayScrollLaneWidth96));
    const int maxScroll = std::max(0, viewerMaxLinePx_ - viewW);
    viewerScrollX_ = std::clamp(viewerScrollX_ + deltaPx, 0, maxScroll);
    if (viewer_)
        InvalidateRect(viewer_, nullptr, FALSE);
}

void AiChangesPane::EnsureViewerRowVisible(size_t row) {
    if (!viewer_ || rows_.empty())
        return;
    RECT client{};
    GetClientRect(viewer_, &client);
    const int bodyH = std::max(1, static_cast<int>(client.bottom - client.top) - HeaderHeightPx());
    const int padY = D(6);
    const int rowH = std::max(D(18), viewerRowHeight_);
    const int top = padY + static_cast<int>(std::min(row, rows_.size() - 1)) * rowH;
    const int bottom = top + rowH;
    if (top < viewerScrollY_)
        viewerScrollY_ = std::max(0, top);
    else if (bottom > viewerScrollY_ + bodyH)
        viewerScrollY_ = std::max(0, bottom - bodyH);
}

RECT AiChangesPane::ViewerScrollbarThumbRect(const RECT &client) const {
    const int headerH = HeaderHeightPx();
    const int h = client.bottom - client.top - headerH;
    const int totalH = D(12) + static_cast<int>(rows_.size()) * std::max(D(18), viewerRowHeight_);
    if (totalH <= h || h <= 0)
        return {};
    const int pad = D(6);
    const int trackH = std::max(1, h - pad * 2);
    const int thumbH = std::clamp(MulDiv(h, trackH, totalH), D(24), trackH);
    const int maxScroll = std::max(1, totalH - h);
    const int y = headerH + pad + MulDiv(viewerScrollY_, std::max(0, trackH - thumbH), maxScroll);
    const int right = client.right - D(4);
    return {right - Ui::OverlayScrollThumbWidth, y, right, y + thumbH};
}

AiChangesPane::TextPoint AiChangesPane::ViewerHitTest(int x, int y) const {
    if (rows_.empty())
        return {};
    const int headerH = HeaderHeightPx();
    const int codeLeft = GutterWidthPx() + D(10);
    const int padY = D(6);
    const int rowH = std::max(D(18), viewerRowHeight_);
    const int rIdx = std::clamp((y - headerH + viewerScrollY_ - padY) / rowH, 0, static_cast<int>(rows_.size()) - 1);
    const auto &row = rows_[static_cast<size_t>(rIdx)];
    if (row.kind == DiffLineKind::Collapsed)
        return {static_cast<size_t>(rIdx), 0};
    const auto &line = row.text;
    const int targetPx = std::max(0, x - codeLeft + viewerScrollX_);

    size_t col = 0;
    if (!line.empty() && viewer_ && codeFont_) {
        HDC dc = GetDC(viewer_);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, codeFont_);
            std::vector<int> extents(line.size(), 0);
            int fit = 0;
            SIZE sz{};
            if (GetTextExtentExPointW(dc, line.c_str(), static_cast<int>(line.size()), targetPx + viewerCharWidth_ * 2,
                                      &fit, extents.data(), &sz)) {
                size_t best = 0;
                int prevRight = 0;
                for (size_t i = 0; i < line.size(); ++i) {
                    const int mid = (prevRight + extents[i]) / 2;
                    if (targetPx < mid)
                        break;
                    best = i + 1;
                    prevRight = extents[i];
                }
                col = best;
            }
            if (old)
                SelectObject(dc, old);
            ReleaseDC(viewer_, dc);
        }
    }
    return {static_cast<size_t>(rIdx), col};
}

std::wstring AiChangesPane::ViewerSelectedText() const {
    if (!HasViewerSelection() || rows_.empty())
        return {};
    const TextPoint lo = selAnchor_ < selFocus_ ? selAnchor_ : selFocus_;
    const TextPoint hi = selAnchor_ < selFocus_ ? selFocus_ : selAnchor_;
    std::wstring out;
    for (size_t r = lo.row; r <= hi.row && r < rows_.size(); ++r) {
        if (rows_[r].kind == DiffLineKind::Collapsed)
            continue;
        const auto &text = rows_[r].text;
        const size_t c0 = (r == lo.row) ? (std::min)(lo.col, text.size()) : 0;
        const size_t c1 = (r == hi.row) ? (std::min)(hi.col, text.size()) : text.size();
        if (c1 > c0)
            out.append(text, c0, c1 - c0);
        if (r < hi.row && rows_[r + 1].logicalIndex != rows_[r].logicalIndex)
            out += L"\r\n";
    }
    return out;
}

std::wstring AiChangesPane::ViewerFullText() const {
    const auto *rec = SelectedRecord();
    if (!rec)
        return {};
    std::string raw;
    if (viewMode_ == ViewMode::Diff)
        raw = !rec->diffText.empty() ? rec->diffText : (!rec->contentPreview.empty() ? rec->contentPreview : rec->detail);
    else
        raw = !rec->contentPreview.empty() ? rec->contentPreview : (!rec->diffText.empty() ? rec->diffText : rec->detail);
    std::wstring normalized;
    normalized.reserve(raw.size() + 16);
    const auto wide = ToWideText(raw);
    for (size_t i = 0; i < wide.size(); ++i) {
        if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r'))
            normalized.push_back(L'\r');
        normalized.push_back(wide[i]);
    }
    return normalized;
}

void AiChangesPane::CopyText(const std::wstring &text, const wchar_t *notice) {
    if (text.empty())
        return;
    if (copyCallback_) {
        copyCallback_(text, notice);
        return;
    }
    if (OpenClipboard(window_)) {
        auto data = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        if (data) {
            if (auto ptr = GlobalLock(data)) {
                memcpy(ptr, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(data);
                EmptyClipboard();
                if (!SetClipboardData(CF_UNICODETEXT, data))
                    GlobalFree(data);
            } else {
                GlobalFree(data);
            }
        }
        CloseClipboard();
    }
}

void AiChangesPane::CopySelectionOrAll() {
    if (HasViewerSelection())
        CopyText(ViewerSelectedText(), L"已复制选中的修改内容");
    else
        CopyText(ViewerFullText(), viewMode_ == ViewMode::Diff ? L"已复制 Diff 内容" : L"已复制文件内容");
}

LRESULT CALLBACK AiChangesPane::SearchEditSubclassProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto *self = reinterpret_cast<AiChangesPane *>(data);
    if (message == WM_PAINT && self && GetWindowTextLengthW(h) == 0) {
        const LRESULT res = DefSubclassProc(h, message, w, l);
        const bool focused = (GetFocus() == h);
        if (focused)
            HideCaret(h);
        if (HDC dc = GetDC(h)) {
            RECT rc{};
            GetClientRect(h, &rc);
            rc.left += self->D(2);
            DrawTextBox(dc, L"search", rc, self->smallFont_, UiAiTheme(self->dark_).muted,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            ReleaseDC(h, dc);
        }
        if (focused)
            ShowCaret(h);
        return res;
    }
    if ((message == WM_SETFOCUS || message == WM_KILLFOCUS) && self && GetWindowTextLengthW(h) == 0) {
        InvalidateRect(h, nullptr, FALSE);
    }
    if (message == WM_KEYDOWN && w == VK_RETURN && self) {
        self->CommitSearchHistory(self->searchQuery_);
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        self->StepSearchMatch(shift ? -1 : 1);
        return 0;
    }
    if (message == WM_KILLFOCUS && self) {
        self->CommitSearchHistory(self->searchQuery_);
    }
    if (message == WM_CHAR && w == L'\r')
        return 0;
    if (message == WM_KEYDOWN && w == VK_ESCAPE && self && self->viewer_) {
        SetFocus(self->viewer_);
        return 0;
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(h, SearchEditSubclassProc, id);
    return DefSubclassProc(h, message, w, l);
}

LRESULT CALLBACK AiChangesPane::HostProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    auto *self = reinterpret_cast<AiChangesPane *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self)
        return DefWindowProcW(h, message, w, l);

    switch (message) {
    case WM_TIMER:
        if (w == 1) {
            self->RefreshGitChanges();
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        self->Layout();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        self->PaintHost(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORBTN: {
        const auto c = UiTheme(self->dark_);
        const COLORREF bg = self->Card1FillColor();
        HDC dc = reinterpret_cast<HDC>(w);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c.text);
        SetBkColor(dc, bg);
        return reinterpret_cast<LRESULT>(self->card1Brush_ ? self->card1Brush_ : GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLOREDIT: {
        const auto c = UiAiTheme(self->dark_);
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, c.text);
        SetBkColor(dc, c.field);
        return reinterpret_cast<LRESULT>(self->fieldBrush_ ? self->fieldBrush_ : GetStockObject(NULL_BRUSH));
    }
    case WM_MEASUREITEM: {
        auto *measure = reinterpret_cast<MEASUREITEMSTRUCT *>(l);
        if (measure && measure->CtlType == ODT_COMBOBOX) {
            measure->itemHeight = self->D(28);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        if (item.CtlType == ODT_COMBOBOX) {
            UiDrawComboItem(item, self->fieldStyle_);
            return TRUE;
        }
        UiBufferedPaint paint(item.hDC, item.rcItem);
        HDC dc = paint.Dc();
        const auto c = UiTheme(self->dark_);
        const COLORREF baseSurface = self->Card1FillColor();
        HBRUSH baseBr = CreateSolidBrush(baseSurface);
        FillRect(dc, &item.rcItem, baseBr);
        DeleteObject(baseBr);

        const bool enabled = IsWindowEnabled(item.hwndItem) != FALSE;
        const bool pressed = enabled && (item.itemState & ODS_SELECTED) != 0;
        const bool hover = GetPropW(item.hwndItem, L"SerialCtl.PowerHover") != nullptr;
        const auto visual = UiFlatIconVisual(c, baseSurface, enabled, pressed, hover);
        if (visual.fill != baseSurface)
            UiBox(dc, item.rcItem, visual.fill, visual.fill, UiCapsuleRadius(item.rcItem));
        UiButtonKeyboardFocus(dc, item.rcItem, item.itemState, self->dpi_, UiFocusInk(self->dark_, false));
        if (!UiDrawButtonGlyph(dc, item, visual.ink, self->dpi_)) {
            DrawTextBox(dc, ControlCaption(item.hwndItem), UiCapsuleContentRect(item.rcItem, self->dpi_),
                        self->uiFont_ ? self->uiFont_ : self->smallFont_, visual.ink,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(w);
        const int code = HIWORD(w);
        if (code == BN_CLICKED && id == PrevMatchButton) {
            self->StepSearchMatch(-1);
            return 0;
        }
        if (code == BN_CLICKED && id == NextMatchButton) {
            self->StepSearchMatch(1);
            return 0;
        }
        if (id == ModeComboBox && code == CBN_SELCHANGE && self->modeCombo_) {
            const int sel = static_cast<int>(SendMessageW(self->modeCombo_, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel <= static_cast<int>(Scope::GitHistory)) {
                self->SetScope(static_cast<Scope>(sel));
                self->SetViewMode(ViewMode::Diff);
            }
            return 0;
        }
        if (id == FileComboBox && code == CBN_SELCHANGE && self->fileCombo_) {
            const int sel = static_cast<int>(SendMessageW(self->fileCombo_, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && static_cast<size_t>(sel) < self->filteredIndices_.size()) {
                const auto &rec = self->VisibleRecords()[self->filteredIndices_[static_cast<size_t>(sel)]];
                if (rec.changeId != self->selectedChangeId_) {
                    self->selectedChangeId_ = rec.changeId;
                    if (self->IsGitScope())
                        self->nextGitRefresh_ = 0;
                    self->viewerScrollY_ = 0;
                    self->viewerScrollX_ = 0;
                    self->hasSelection_ = false;
                    self->UpdateButtons();
                    self->RebuildLogicalLines();
                    self->RebuildDisplayRows();
                    self->RebuildSearchMatches(!self->searchQuery_.empty());
                    if (self->viewer_)
                        InvalidateRect(self->viewer_, nullptr, FALSE);
                }
            }
            return 0;
        }
        if (id == SearchComboBox && !self->suppressSearchNotify_ && self->searchCombo_) {
            if (code == CBN_EDITCHANGE) {
                self->SetSearchQuery(ControlCaption(self->searchCombo_));
                return 0;
            }
            if (code == CBN_SELCHANGE || code == CBN_SELENDOK) {
                const LRESULT sel = SendMessageW(self->searchCombo_, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
                    const LRESULT len = SendMessageW(self->searchCombo_, CB_GETLBTEXTLEN, sel, 0);
                    if (len > 0) {
                        std::wstring picked(static_cast<size_t>(len) + 1, L'\0');
                        const LRESULT got = SendMessageW(self->searchCombo_, CB_GETLBTEXT, sel,
                                                         reinterpret_cast<LPARAM>(picked.data()));
                        picked.resize(got > 0 ? static_cast<size_t>(got) : 0);
                        self->SetSearchQuery(picked);
                    }
                }
                return 0;
            }
        }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(h, message, w, l);
}

LRESULT CALLBACK AiChangesPane::ViewerProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    auto *self = reinterpret_cast<AiChangesPane *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self)
        return DefWindowProcW(h, message, w, l);

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (self->wrap_)
            self->RebuildDisplayRows();
        self->ScrollViewerY(0);
        self->ScrollViewerX(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        self->PaintViewer(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = -GET_WHEEL_DELTA_WPARAM(w) * std::max(self->D(18), self->viewerRowHeight_) * 3 / WHEEL_DELTA;
        if ((GET_KEYSTATE_WPARAM(w) & MK_SHIFT) && !self->wrap_)
            self->ScrollViewerX(delta);
        else
            self->ScrollViewerY(delta);
        return 0;
    }
    case WM_MOUSEHWHEEL:
        if (!self->wrap_)
            self->ScrollViewerX(GET_WHEEL_DELTA_WPARAM(w) * self->D(24) / WHEEL_DELTA);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        const int x = GET_X_LPARAM(l);
        const int y = GET_Y_LPARAM(l);
        RECT client{};
        GetClientRect(h, &client);
        const RECT thumb = self->ViewerScrollbarThumbRect(client);
        if (thumb.bottom > thumb.top && x >= client.right - self->D(Ui::OverlayScrollLaneWidth96)) {
            if (y >= thumb.top && y <= thumb.bottom) {
                self->viewerDraggingThumb_ = true;
                self->viewerDragGrabOffset_ = y - thumb.top;
                SetCapture(h);
            } else {
                const int bodyH = static_cast<int>(client.bottom - client.top) - self->HeaderHeightPx();
                const int page = std::max(self->viewerRowHeight_, bodyH - self->viewerRowHeight_);
                self->ScrollViewerY(y < thumb.top ? -page : page);
            }
            return 0;
        }
        const int headerH = self->HeaderHeightPx();
        if (y < headerH)
            return 0;

        const TextPoint pt = self->ViewerHitTest(x, y);
        if (pt.row < self->rows_.size() && self->rows_[pt.row].kind == DiffLineKind::Collapsed) {
            const size_t blockId = self->rows_[pt.row].collapsedIndex;
            if (blockId != SIZE_MAX)
                self->ExpandCollapsedBlock(blockId);
            return 0;
        }
        if ((w & MK_SHIFT) && self->hasSelection_) {
            self->selFocus_ = pt;
        } else {
            self->selAnchor_ = pt;
            self->selFocus_ = pt;
            self->hasSelection_ = false;
        }
        self->selectingText_ = true;
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const int x = GET_X_LPARAM(l);
        const int y = GET_Y_LPARAM(l);
        if (y < self->HeaderHeightPx())
            return 0;
        const TextPoint pt = self->ViewerHitTest(x, y);
        if (pt.row < self->rows_.size()) {
            const auto &row = self->rows_[pt.row];
            if (row.kind == DiffLineKind::Collapsed) {
                if (row.collapsedIndex != SIZE_MAX)
                    self->ExpandCollapsedBlock(row.collapsedIndex);
                return 0;
            }
            const auto &text = row.text;
            if (!text.empty()) {
                size_t lo = (std::min)(pt.col, text.size() - 1);
                size_t hi = lo;
                if (IsWordChar(text[lo])) {
                    while (lo > 0 && IsWordChar(text[lo - 1]))
                        --lo;
                    while (hi + 1 < text.size() && IsWordChar(text[hi + 1]))
                        ++hi;
                    ++hi;
                } else {
                    lo = 0;
                    hi = text.size();
                }
                self->selAnchor_ = {pt.row, lo};
                self->selFocus_ = {pt.row, hi};
                self->hasSelection_ = (lo != hi);
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(l);
        const int y = GET_Y_LPARAM(l);
        if (self->viewerDraggingThumb_) {
            RECT client{};
            GetClientRect(h, &client);
            const RECT thumb = self->ViewerScrollbarThumbRect(client);
            const int bodyH = std::max(1, static_cast<int>(client.bottom - client.top) - self->HeaderHeightPx());
            const int totalH = self->D(12) + static_cast<int>(self->rows_.size()) * std::max(self->D(18), self->viewerRowHeight_);
            const int pad = self->D(6);
            const int trackH = std::max<int>(1, bodyH - pad * 2 - (thumb.bottom - thumb.top));
            const int maxScroll = std::max(0, totalH - bodyH);
            const int newTop = std::clamp(y - self->viewerDragGrabOffset_ - (self->HeaderHeightPx() + pad), 0, trackH);
            self->viewerScrollY_ = trackH > 0 ? MulDiv(newTop, maxScroll, trackH) : 0;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (self->selectingText_) {
            RECT client{};
            GetClientRect(h, &client);
            if (y < self->HeaderHeightPx())
                self->ScrollViewerY(-self->viewerRowHeight_);
            else if (y > client.bottom)
                self->ScrollViewerY(self->viewerRowHeight_);
            self->selFocus_ = self->ViewerHitTest(x, y);
            self->hasSelection_ = (self->selAnchor_ != self->selFocus_);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        if (self->viewerDraggingThumb_) {
            self->viewerDraggingThumb_ = false;
            ReleaseCapture();
        }
        if (self->selectingText_) {
            self->selectingText_ = false;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        self->viewerDraggingThumb_ = false;
        self->selectingText_ = false;
        return 0;
    case WM_RBUTTONUP:
        self->CopySelectionOrAll();
        return 0;
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && (w == 'F' || w == 'f') && self->searchEdit_) {
            SetFocus(self->searchEdit_);
            SendMessageW(self->searchEdit_, EM_SETSEL, 0, -1);
            return 0;
        }
        if (ctrl && (w == 'C' || w == 'c')) {
            self->CopySelectionOrAll();
            return 0;
        }
        if (ctrl && (w == 'A' || w == 'a') && !self->rows_.empty()) {
            self->selAnchor_ = {0, 0};
            self->selFocus_ = {self->rows_.size() - 1, self->rows_.back().text.size()};
            self->hasSelection_ = (self->selAnchor_ != self->selFocus_);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_F3) {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            self->StepSearchMatch(shift ? -1 : 1);
            return 0;
        }
        RECT client{};
        GetClientRect(h, &client);
        const int bodyH = static_cast<int>(client.bottom - client.top) - self->HeaderHeightPx();
        const int page = std::max(self->viewerRowHeight_, bodyH - self->viewerRowHeight_);
        if (w == VK_UP)
            self->ScrollViewerY(-self->viewerRowHeight_);
        else if (w == VK_DOWN)
            self->ScrollViewerY(self->viewerRowHeight_);
        else if (w == VK_PRIOR)
            self->ScrollViewerY(-page);
        else if (w == VK_NEXT)
            self->ScrollViewerY(page);
        else if (w == VK_HOME)
            self->ScrollViewerY(-10000000);
        else if (w == VK_END)
            self->ScrollViewerY(10000000);
        else
            break;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, message, w, l);
}

} // namespace serialctl
