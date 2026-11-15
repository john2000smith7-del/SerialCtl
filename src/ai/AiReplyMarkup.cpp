#include "AiReplyMarkup.h"
#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>
#include <utility>

namespace serialctl::ai::markup {
namespace {

using View = std::wstring_view;

// A short row with few cells can otherwise expand to millions of empty
// native cells when the reply has an exceptionally wide header.
constexpr size_t MaxTableCells = 32768;
constexpr int MaxInlineDepth = 8;
constexpr int MaxParseDepth = 4;

bool Starts(View text, View prefix) { return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix; }
bool Space(wchar_t c) { return c == L' ' || c == L'\t'; }

bool StartsNoCase(View text, View prefix) {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::towlower(text[i]) != std::towlower(prefix[i])) return false;
    return true;
}

bool EqualsNoCase(View a, View b) {
    return a.size() == b.size() && StartsNoCase(a, b);
}

View Trim(View text) {
    while (!text.empty() && Space(text.front())) text.remove_prefix(1);
    while (!text.empty() && Space(text.back())) text.remove_suffix(1);
    return text;
}

View TrimAllSpace(View text) {
    auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!text.empty() && ws(text.front())) text.remove_prefix(1);
    while (!text.empty() && ws(text.back())) text.remove_suffix(1);
    return text;
}

std::wstring Lower(View value) {
    std::wstring result(value);
    for (auto &c : result) c = static_cast<wchar_t>(std::towlower(c));
    return result;
}

bool Escaped(View text, size_t index) {
    size_t count = 0;
    while (index && text[--index] == L'\\') ++count;
    return (count & 1) != 0;
}

bool AsciiPunctuation(wchar_t c) {
    constexpr View punct = L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    return punct.find(c) != View::npos;
}

bool AsciiWordChar(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_';
}

size_t Run(View text, size_t index, wchar_t c) {
    size_t end = index;
    while (end < text.size() && text[end] == c) ++end;
    return end - index;
}

size_t ClosingTicks(View text, size_t start, size_t length) {
    for (size_t i = text.find(L'`', start); i != View::npos;) {
        const auto run = Run(text, i, L'`');
        if (run == length) return i;
        i = text.find(L'`', i + run);
    }
    return View::npos;
}

size_t LeadingIndentCols(View text, size_t *consumedChars = nullptr) {
    size_t cols = 0, i = 0;
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) {
        cols += text[i] == L'\t' ? 4 : 1;
        ++i;
    }
    if (consumedChars) *consumedChars = i;
    return cols;
}

Ink PrimaryInkForFlags(unsigned flags) {
    if (flags & InlineCodeFlag) return Ink::InlineCode;
    if (flags & MathFlag) return Ink::Math;
    if (flags & KbdFlag) return Ink::Kbd;
    if (flags & LinkFlag) return Ink::Link;
    if (flags & MarkFlag) return Ink::Mark;
    if ((flags & BoldFlag) && (flags & ItalicFlag)) return Ink::BoldItalic;
    if (flags & BoldFlag) return Ink::Bold;
    if (flags & ItalicFlag) return Ink::Italic;
    if (flags & StrikeFlag) return Ink::Strike;
    if (flags & UnderlineFlag) return Ink::Underline;
    if (flags & MutedFlag) return Ink::Muted;
    if (flags & SubFlag) return Ink::Sub;
    if (flags & SupFlag) return Ink::Sup;
    return Ink::Plain;
}

void EmitRun(Text &result, View chunk, unsigned flags, View linkUrl) {
    if (chunk.empty()) return;
    const size_t start = result.value.size();
    result.value.append(chunk);
    if (flags != 0) {
        const Ink ink = PrimaryInkForFlags(flags);
        if (!result.spans.empty() && result.spans.back().start + result.spans.back().length == start &&
            result.spans.back().ink == ink && result.spans.back().flags == flags) {
            result.spans.back().length += chunk.size();
        } else {
            result.spans.push_back({start, chunk.size(), ink, flags});
        }
    }
    if ((flags & LinkFlag) && !linkUrl.empty()) {
        if (!result.links.empty() && result.links.back().start + result.links.back().length == start &&
            result.links.back().url == linkUrl) {
            result.links.back().length += chunk.size();
        } else {
            result.links.push_back({start, chunk.size(), std::wstring(linkUrl)});
        }
    }
}

std::optional<std::pair<std::wstring, size_t>> DecodeEntity(View text) {
    if (text.size() < 4 || text.front() != L'&') return {};
    const size_t limit = std::min<size_t>(text.size(), 12);
    const size_t semi = text.substr(0, limit).find(L';');
    if (semi == View::npos || semi < 2) return {};
    const View name = text.substr(1, semi - 1);
    if (name.front() == L'#') {
        if (name.size() < 2) return {};
        const bool hex = name[1] == L'x' || name[1] == L'X';
        const View digits = name.substr(hex ? 2 : 1);
        if (digits.empty()) return {};
        unsigned long cp = 0;
        for (wchar_t ch : digits) {
            unsigned d = 0;
            if (ch >= L'0' && ch <= L'9') d = static_cast<unsigned>(ch - L'0');
            else if (hex && ch >= L'a' && ch <= L'f') d = static_cast<unsigned>(ch - L'a' + 10);
            else if (hex && ch >= L'A' && ch <= L'F') d = static_cast<unsigned>(ch - L'A' + 10);
            else return {};
            cp = cp * (hex ? 16u : 10u) + d;
            if (cp > 0x10FFFFu) return {};
        }
        if (cp == 0 || (cp >= 0xD800u && cp <= 0xDFFFu) || (cp < 0x20u && cp != 0x09u)) return {};
        std::wstring out;
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            cp -= 0x10000u;
            out.push_back(static_cast<wchar_t>(0xD800u + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu)));
        }
        return std::make_pair(std::move(out), semi + 1);
    }
    struct Entry { View entity, value; };
    static constexpr std::array<Entry, 20> named{{
        {L"amp", L"&"},       {L"lt", L"<"},       {L"gt", L">"},        {L"quot", L"\""},
        {L"apos", L"'"},      {L"nbsp", L" "},     {L"copy", L"\u00A9"}, {L"reg", L"\u00AE"},
        {L"trade", L"\u2122"},{L"mdash", L"\u2014"},{L"ndash", L"\u2013"},{L"hellip", L"\u2026"},
        {L"times", L"\u00D7"},{L"divide", L"\u00F7"},{L"plusmn", L"\u00B1"},{L"middot", L"\u00B7"},
        {L"bull", L"\u2022"}, {L"laquo", L"\u00AB"},{L"raquo", L"\u00BB"},{L"deg", L"\u00B0"},
    }};
    for (const auto &entry : named) {
        if (name == entry.entity) return std::make_pair(std::wstring(entry.value), semi + 1);
    }
    return {};
}

size_t MatchBrTag(View text) {
    if (!StartsNoCase(text, L"<br")) return 0;
    size_t i = 3;
    while (i < text.size() && Space(text[i])) ++i;
    if (i < text.size() && text[i] == L'/') {
        ++i;
        while (i < text.size() && Space(text[i])) ++i;
    }
    if (i < text.size() && text[i] == L'>') return i + 1;
    return 0;
}

std::optional<std::pair<std::wstring, size_t>> MatchAutolink(View text) {
    if (text.size() < 5 || text.front() != L'<') return {};
    const size_t close = text.find(L'>', 1);
    if (close == View::npos) return {};
    const View inside = text.substr(1, close - 1);
    for (wchar_t ch : inside) {
        if (Space(ch) || ch == L'<' || ch == L'\r' || ch == L'\n') return {};
    }
    if (StartsNoCase(inside, L"http://") || StartsNoCase(inside, L"https://") || StartsNoCase(inside, L"mailto:")) {
        if (inside.size() <= 8) return {};
        return std::make_pair(std::wstring(inside), close + 1);
    }
    const size_t at = inside.find(L'@');
    if (at != View::npos && at > 0 && at + 3 < inside.size()) {
        const View domain = inside.substr(at + 1);
        const size_t dot = domain.find(L'.');
        if (dot != View::npos && dot > 0 && dot + 1 < domain.size())
            return std::make_pair(L"mailto:" + std::wstring(inside), close + 1);
    }
    return {};
}

std::optional<std::wstring> ExtractHtmlAttr(View attrs, View attrName) {
    for (size_t i = 0; i < attrs.size();) {
        while (i < attrs.size() && Space(attrs[i])) ++i;
        const size_t nameStart = i;
        while (i < attrs.size() && (std::iswalnum(attrs[i]) || attrs[i] == L'-')) ++i;
        const View name = attrs.substr(nameStart, i - nameStart);
        while (i < attrs.size() && Space(attrs[i])) ++i;
        if (i >= attrs.size() || attrs[i] != L'=') {
            while (i < attrs.size() && !Space(attrs[i])) ++i;
            continue;
        }
        ++i;
        while (i < attrs.size() && Space(attrs[i])) ++i;
        if (i >= attrs.size()) break;
        View value;
        if (attrs[i] == L'"' || attrs[i] == L'\'') {
            const wchar_t q = attrs[i++];
            const size_t close = attrs.find(q, i);
            if (close == View::npos) break;
            value = attrs.substr(i, close - i);
            i = close + 1;
        } else {
            const size_t valStart = i;
            while (i < attrs.size() && !Space(attrs[i])) ++i;
            value = attrs.substr(valStart, i - valStart);
        }
        if (EqualsNoCase(name, attrName)) return std::wstring(value);
    }
    return {};
}

unsigned HtmlInlineTagFlag(View tagLower, View attrs) {
    if (tagLower == L"b" || tagLower == L"strong") return BoldFlag;
    if (tagLower == L"i" || tagLower == L"em") return ItalicFlag;
    if (tagLower == L"u" || tagLower == L"ins") return UnderlineFlag;
    if (tagLower == L"s" || tagLower == L"del" || tagLower == L"strike") return StrikeFlag;
    if (tagLower == L"mark") return MarkFlag;
    if (tagLower == L"kbd") return KbdFlag;
    if (tagLower == L"code") return InlineCodeFlag;
    if (tagLower == L"sub") return SubFlag;
    if (tagLower == L"sup") return SupFlag;
    if (tagLower == L"a") return LinkFlag;
    if (tagLower == L"span" || tagLower == L"font") {
        if (const auto style = ExtractHtmlAttr(attrs, L"style")) {
            const auto low = Lower(*style);
            if (low.find(L"gray") != std::wstring::npos || low.find(L"grey") != std::wstring::npos ||
                low.find(L"muted") != std::wstring::npos)
                return MutedFlag;
        }
        return 0;
    }
    return UINT_MAX;
}

struct PairedHtmlMatch {
    std::wstring tagLower;
    unsigned addedFlag = 0;
    std::wstring linkUrl;
    View inner;
    size_t consumed = 0;
};

std::optional<PairedHtmlMatch> MatchPairedHtmlTag(View text) {
    if (text.size() < 6 || text[0] != L'<' || text[1] == L'/' || text[1] == L'!') return {};
    size_t i = 1;
    while (i < text.size() && (std::iswalnum(text[i]) || text[i] == L'-')) ++i;
    if (i == 1 || i >= text.size()) return {};
    if (!Space(text[i]) && text[i] != L'>') return {};
    const auto tagLower = Lower(text.substr(1, i - 1));
    size_t gt = View::npos;
    wchar_t quote = 0;
    for (size_t pos = i; pos < text.size(); ++pos) {
        const wchar_t ch = text[pos];
        if (quote) {
            if (ch == quote) quote = 0;
        } else if (ch == L'"' || ch == L'\'') {
            quote = ch;
        } else if (ch == L'>') {
            gt = pos;
            break;
        } else if (ch == L'<') {
            return {};
        }
    }
    if (gt == View::npos) return {};
    if (gt > 0 && text[gt - 1] == L'/') return {};
    const View attrs = text.substr(i, gt - i);
    const unsigned addedFlag = HtmlInlineTagFlag(tagLower, attrs);
    if (addedFlag == UINT_MAX) return {};

    std::wstring href;
    if (tagLower == L"a") {
        if (auto attr = ExtractHtmlAttr(attrs, L"href")) href = *attr;
    }

    int depth = 1;
    for (size_t pos = gt + 1; pos < text.size();) {
        if (text[pos] == L'`') {
            const auto ticks = Run(text, pos, L'`');
            const auto closeTicks = ClosingTicks(text, pos + ticks, ticks);
            pos = closeTicks == View::npos ? pos + ticks : closeTicks + ticks;
            continue;
        }
        if (text[pos] != L'<') {
            ++pos;
            continue;
        }
        if (pos + 2 + tagLower.size() < text.size() && text[pos + 1] == L'/') {
            if (StartsNoCase(text.substr(pos + 2), tagLower)) {
                size_t end = pos + 2 + tagLower.size();
                while (end < text.size() && Space(text[end])) ++end;
                if (end < text.size() && text[end] == L'>') {
                    --depth;
                    if (depth == 0) {
                        return PairedHtmlMatch{tagLower, addedFlag, std::move(href),
                                               text.substr(gt + 1, pos - (gt + 1)), end + 1};
                    }
                    pos = end + 1;
                    continue;
                }
            }
        } else if (pos + 1 + tagLower.size() < text.size() && StartsNoCase(text.substr(pos + 1), tagLower)) {
            const size_t after = pos + 1 + tagLower.size();
            if (after < text.size() && (Space(text[after]) || text[after] == L'>')) {
                const size_t innerGt = text.find(L'>', after);
                if (innerGt != View::npos && !(innerGt > 0 && text[innerGt - 1] == L'/')) {
                    ++depth;
                    pos = innerGt + 1;
                    continue;
                }
            }
        }
        ++pos;
    }
    return {};
}

void EmitPlainWithEntitiesAndBr(Text &result, View text, unsigned flags, View linkUrl) {
    for (size_t i = 0; i < text.size();) {
        if (const size_t brLen = MatchBrTag(text.substr(i))) {
            EmitRun(result, L"\n", flags, linkUrl);
            i += brLen;
            continue;
        }
        if (text[i] == L'&') {
            if (const auto ent = DecodeEntity(text.substr(i))) {
                EmitRun(result, ent->first, flags, linkUrl);
                i += ent->second;
                continue;
            }
        }
        size_t next = i + 1;
        while (next < text.size() && text[next] != L'<' && text[next] != L'&') ++next;
        EmitRun(result, text.substr(i, next - i), flags, linkUrl);
        i = next;
    }
}

size_t FindMatchingBracket(View text, size_t openBracket) {
    int depth = 1;
    for (size_t i = openBracket + 1; i < text.size();) {
        if (text[i] == L'\\' && i + 1 < text.size() && AsciiPunctuation(text[i + 1])) {
            i += 2;
            continue;
        }
        if (text[i] == L'`') {
            const auto ticks = Run(text, i, L'`');
            const auto close = ClosingTicks(text, i + ticks, ticks);
            i = close == View::npos ? i + ticks : close + ticks;
            continue;
        }
        if (text[i] == L'[') ++depth;
        else if (text[i] == L']' && --depth == 0) return i;
        ++i;
    }
    return View::npos;
}

struct ParsedLink {
    View label;
    std::wstring url;
    size_t consumed = 0;
};

std::optional<ParsedLink> MatchLinkOrImage(View text, size_t bracketPos) {
    const size_t closeBracket = FindMatchingBracket(text, bracketPos);
    if (closeBracket == View::npos || closeBracket + 1 >= text.size() || text[closeBracket + 1] != L'(') return {};
    const size_t destStart = closeBracket + 2;
    int parenDepth = 1;
    wchar_t quote = 0;
    size_t closeParen = View::npos;
    for (size_t i = destStart; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\\' && i + 1 < text.size()) {
            ++i;
            continue;
        }
        if (quote) {
            if (ch == quote) quote = 0;
            continue;
        }
        if (ch == L'"' || ch == L'\'') {
            quote = ch;
        } else if (ch == L'(') {
            ++parenDepth;
        } else if (ch == L')') {
            if (--parenDepth == 0) {
                closeParen = i;
                break;
            }
        }
    }
    if (closeParen == View::npos) return {};
    View rawDest = Trim(text.substr(destStart, closeParen - destStart));
    if (rawDest.empty()) return {};
    if (rawDest.front() == L'<') {
        const size_t gt = rawDest.find(L'>');
        if (gt != View::npos) rawDest = rawDest.substr(1, gt - 1);
    } else {
        for (size_t i = 0; i < rawDest.size(); ++i) {
            if (Space(rawDest[i])) {
                rawDest = rawDest.substr(0, i);
                break;
            }
        }
    }
    if (rawDest.empty()) return {};
    return ParsedLink{text.substr(bracketPos + 1, closeBracket - bracketPos - 1), std::wstring(rawDest),
                      closeParen + 1};
}

size_t FindClosingEmphasis(View text, size_t start, wchar_t marker, size_t count, bool underscoreBoundary) {
    for (size_t i = start; i < text.size();) {
        if (text[i] == L'\\' && i + 1 < text.size() && AsciiPunctuation(text[i + 1])) {
            i += 2;
            continue;
        }
        if (text[i] == L'`') {
            const auto ticks = Run(text, i, L'`');
            const auto close = ClosingTicks(text, i + ticks, ticks);
            i = close == View::npos ? i + ticks : close + ticks;
            continue;
        }
        if (text[i] == marker) {
            const size_t run = Run(text, i, marker);
            if (run >= count && i > start && !Space(text[i - 1])) {
                const size_t endPos = i + count;
                if (!underscoreBoundary || endPos >= text.size() || !AsciiWordChar(text[endPos])) return i;
            }
            i += run;
            continue;
        }
        ++i;
    }
    return View::npos;
}

size_t FindClosingTildes(View text, size_t start) {
    for (size_t i = start; i < text.size();) {
        if (text[i] == L'\\' && i + 1 < text.size() && AsciiPunctuation(text[i + 1])) {
            i += 2;
            continue;
        }
        if (text[i] == L'`') {
            const auto ticks = Run(text, i, L'`');
            const auto close = ClosingTicks(text, i + ticks, ticks);
            i = close == View::npos ? i + ticks : close + ticks;
            continue;
        }
        if (Starts(text.substr(i), L"~~") && i > start && !Space(text[i - 1])) return i;
        ++i;
    }
    return View::npos;
}

bool MathAsciiLetter(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

void EmitMathSpace(Text &result, unsigned flags, View linkUrl) {
    if (result.value.empty() || result.value.back() == L' ' || result.value.back() == L'\n') return;
    EmitRun(result, L" ", flags, linkUrl);
}

View ReadMathGroup(View tex, size_t &pos) {
    while (pos < tex.size() && Space(tex[pos])) ++pos;
    if (pos >= tex.size()) return {};
    if (tex[pos] == L'{') {
        int depth = 1;
        size_t i = pos + 1;
        while (i < tex.size() && depth > 0) {
            if (tex[i] == L'\\' && i + 1 < tex.size()) {
                i += 2;
                continue;
            }
            if (tex[i] == L'{') ++depth;
            else if (tex[i] == L'}' && --depth == 0) break;
            ++i;
        }
        const View inner = tex.substr(pos + 1, i - (pos + 1));
        pos = i < tex.size() ? i + 1 : tex.size();
        return inner;
    }
    if (tex[pos] == L'\\') {
        size_t end = pos + 1;
        if (end < tex.size() && MathAsciiLetter(tex[end])) {
            while (end < tex.size() && MathAsciiLetter(tex[end])) ++end;
        } else if (end < tex.size()) {
            ++end;
        }
        const View token = tex.substr(pos, end - pos);
        pos = end;
        return token;
    }
    const View single = tex.substr(pos, 1);
    ++pos;
    return single;
}

std::optional<View> LookupMathSymbol(View cmd) {
    struct Entry { View cmd, out; };
    static constexpr Entry table[] = {
        {L"in", L"\u2208"}, {L"notin", L"\u2209"}, {L"ni", L"\u220B"},
        {L"subset", L"\u2282"}, {L"supset", L"\u2283"}, {L"subseteq", L"\u2286"}, {L"supseteq", L"\u2287"},
        {L"cup", L"\u222A"}, {L"cap", L"\u2229"}, {L"setminus", L"\u2216"},
        {L"emptyset", L"\u2205"}, {L"varnothing", L"\u2205"},
        {L"land", L"\u2227"}, {L"wedge", L"\u2227"}, {L"lor", L"\u2228"}, {L"vee", L"\u2228"},
        {L"neg", L"\u00AC"}, {L"lnot", L"\u00AC"},
        {L"to", L"\u2192"}, {L"rightarrow", L"\u2192"}, {L"gets", L"\u2190"}, {L"leftarrow", L"\u2190"},
        {L"leftrightarrow", L"\u2194"}, {L"mapsto", L"\u21A6"},
        {L"implies", L"\u21D2"}, {L"Rightarrow", L"\u21D2"}, {L"Leftarrow", L"\u21D0"},
        {L"iff", L"\u21D4"}, {L"Leftrightarrow", L"\u21D4"},
        {L"forall", L"\u2200"}, {L"exists", L"\u2203"}, {L"nexists", L"\u2204"},
        {L"ll", L"\u226A"}, {L"gg", L"\u226B"},
        {L"le", L"\u2264"}, {L"leq", L"\u2264"}, {L"ge", L"\u2265"}, {L"geq", L"\u2265"},
        {L"neq", L"\u2260"}, {L"ne", L"\u2260"},
        {L"approx", L"\u2248"}, {L"simeq", L"\u2243"}, {L"cong", L"\u2245"}, {L"equiv", L"\u2261"},
        {L"propto", L"\u221D"}, {L"sim", L"\u223C"},
        {L"times", L"\u00D7"}, {L"div", L"\u00F7"}, {L"cdot", L"\u00B7"},
        {L"ast", L"\u2217"}, {L"star", L"\u22C6"}, {L"pm", L"\u00B1"}, {L"mp", L"\u2213"},
        {L"oplus", L"\u2295"}, {L"otimes", L"\u2297"}, {L"circ", L"\u2218"}, {L"bullet", L"\u2022"},
        {L"infty", L"\u221E"}, {L"partial", L"\u2202"}, {L"nabla", L"\u2207"},
        {L"sum", L"\u2211"}, {L"prod", L"\u220F"}, {L"int", L"\u222B"}, {L"oint", L"\u222E"},
        {L"angle", L"\u2220"}, {L"perp", L"\u22A5"}, {L"parallel", L"\u2225"}, {L"triangle", L"\u25B3"},
        {L"therefore", L"\u2234"}, {L"because", L"\u2235"},
        {L"dots", L"\u2026"}, {L"ldots", L"\u2026"}, {L"cdots", L"\u22EF"}, {L"vdots", L"\u22EE"},
        {L"langle", L"\u27E8"}, {L"rangle", L"\u27E9"},
        {L"lceil", L"\u2308"}, {L"rceil", L"\u2309"}, {L"lfloor", L"\u230A"}, {L"rfloor", L"\u230B"},
        {L"mid", L"|"},
        {L"alpha", L"\u03B1"}, {L"beta", L"\u03B2"}, {L"gamma", L"\u03B3"}, {L"delta", L"\u03B4"},
        {L"epsilon", L"\u03B5"}, {L"varepsilon", L"\u03B5"}, {L"zeta", L"\u03B6"}, {L"eta", L"\u03B7"},
        {L"theta", L"\u03B8"}, {L"vartheta", L"\u03B8"}, {L"iota", L"\u03B9"}, {L"kappa", L"\u03BA"},
        {L"lambda", L"\u03BB"}, {L"mu", L"\u03BC"}, {L"nu", L"\u03BD"}, {L"xi", L"\u03BE"},
        {L"pi", L"\u03C0"}, {L"varpi", L"\u03D6"}, {L"rho", L"\u03C1"}, {L"varrho", L"\u03F1"},
        {L"sigma", L"\u03C3"}, {L"varsigma", L"\u03C2"}, {L"tau", L"\u03C4"}, {L"upsilon", L"\u03C5"},
        {L"phi", L"\u03C6"}, {L"varphi", L"\u03C6"}, {L"chi", L"\u03C7"}, {L"psi", L"\u03C8"},
        {L"omega", L"\u03C9"},
        {L"Gamma", L"\u0393"}, {L"Delta", L"\u0394"}, {L"Theta", L"\u0398"}, {L"Lambda", L"\u039B"},
        {L"Xi", L"\u039E"}, {L"Pi", L"\u03A0"}, {L"Sigma", L"\u03A3"}, {L"Upsilon", L"\u03A5"},
        {L"Phi", L"\u03A6"}, {L"Psi", L"\u03A8"}, {L"Omega", L"\u03A9"},
    };
    for (const auto &entry : table) {
        if (entry.cmd == cmd) return entry.out;
    }
    return {};
}

bool IsMathFunctionName(View cmd) {
    static constexpr View funcs[] = {
        L"sin", L"cos", L"tan", L"cot", L"sec", L"csc",
        L"arcsin", L"arccos", L"arctan", L"sinh", L"cosh", L"tanh",
        L"log", L"ln", L"lg", L"exp", L"lim", L"sup", L"inf",
        L"max", L"min", L"arg", L"det", L"dim", L"ker", L"Pr", L"deg", L"gcd", L"mod", L"bmod"
    };
    for (View f : funcs) {
        if (f == cmd) return true;
    }
    return false;
}

bool NeedsMathFractionParens(View raw) {
    const View s = Trim(raw);
    if (s.empty()) return false;
    if (s.size() >= 2 && s.front() == L'(' && s.back() == L')') return false;
    int depth = 0;
    for (wchar_t ch : s) {
        if (ch == L'{' || ch == L'(' || ch == L'[') ++depth;
        else if ((ch == L'}' || ch == L')' || ch == L']') && depth > 0) --depth;
        else if (depth == 0 && (Space(ch) || ch == L'+' || ch == L'-' || ch == L'*' || ch == L'/' || ch == L'='))
            return true;
    }
    return false;
}

void AppendMathTex(Text &result, View tex, unsigned flags, View linkUrl, bool plainText = false, int depth = 0) {
    if (depth >= MaxInlineDepth) {
        EmitRun(result, tex, flags, linkUrl);
        return;
    }
    for (size_t i = 0; i < tex.size();) {
        const wchar_t ch = tex[i];
        if (Space(ch)) {
            if (plainText) EmitRun(result, tex.substr(i, 1), flags, linkUrl);
            else EmitMathSpace(result, flags, linkUrl);
            ++i;
            continue;
        }
        if (ch == L'~') {
            EmitMathSpace(result, flags, linkUrl);
            ++i;
            continue;
        }
        if (ch == L'&' && !plainText) {
            EmitMathSpace(result, flags, linkUrl);
            ++i;
            continue;
        }
        if (ch == L'{' || ch == L'}') {
            ++i;
            continue;
        }
        if ((ch == L'_' || ch == L'^') && !plainText) {
            size_t next = i + 1;
            const View arg = ReadMathGroup(tex, next);
            const unsigned scriptFlag = ch == L'_' ? SubFlag : SupFlag;
            const unsigned cleared = flags & ~(SubFlag | SupFlag | ItalicFlag);
            if (!arg.empty()) AppendMathTex(result, arg, cleared | scriptFlag, linkUrl, false, depth + 1);
            i = next;
            continue;
        }
        if (ch == L'\\') {
            if (i + 1 >= tex.size()) {
                EmitRun(result, L"\\", flags, linkUrl);
                ++i;
                continue;
            }
            const wchar_t esc = tex[i + 1];
            if (!MathAsciiLetter(esc)) {
                i += 2;
                if (esc == L'\\') {
                    EmitRun(result, L"\n", flags, linkUrl);
                    while (i < tex.size() && Space(tex[i])) ++i;
                } else if (esc == L';' || esc == L':' || esc == L',' || esc == L' ') EmitMathSpace(result, flags, linkUrl);
                else if (esc == L'!') { /* negative thin space ignored */ }
                else if (esc == L'|') EmitRun(result, L"\u2016", flags, linkUrl);
                else EmitRun(result, View(&esc, 1), flags, linkUrl);
                continue;
            }
            size_t end = i + 1;
            while (end < tex.size() && MathAsciiLetter(tex[end])) ++end;
            const View cmd = tex.substr(i + 1, end - (i + 1));
            i = end;
            if (cmd == L"quad" || cmd == L"qquad" || cmd == L"enspace" || cmd == L"thinspace") {
                EmitMathSpace(result, flags, linkUrl);
            } else if (cmd == L"left" || cmd == L"right" || cmd == L"big" || cmd == L"Big" ||
                       cmd == L"bigg" || cmd == L"Bigg") {
                if (i < tex.size() && tex[i] == L'.') ++i;
            } else if (cmd == L"begin" || cmd == L"end") {
                if (i < tex.size() && (tex[i] == L'{' || Space(tex[i]))) (void)ReadMathGroup(tex, i);
                while (i < tex.size() && (tex[i] == L'*' || Space(tex[i]))) ++i;
            } else if (cmd == L"text" || cmd == L"textrm" || cmd == L"mathrm" ||
                       cmd == L"operatorname" || cmd == L"mbox" || cmd == L"mathsf" || cmd == L"mathcal") {
                const View inner = ReadMathGroup(tex, i);
                if (!inner.empty()) AppendMathTex(result, inner, flags & ~ItalicFlag, linkUrl, true, depth + 1);
            } else if (cmd == L"mathbf" || cmd == L"boldsymbol" || cmd == L"bm") {
                const View inner = ReadMathGroup(tex, i);
                if (!inner.empty()) AppendMathTex(result, inner, (flags & ~ItalicFlag) | BoldFlag, linkUrl, false, depth + 1);
            } else if (cmd == L"mathit") {
                const View inner = ReadMathGroup(tex, i);
                if (!inner.empty()) AppendMathTex(result, inner, flags | ItalicFlag, linkUrl, false, depth + 1);
            } else if (cmd == L"texttt") {
                const View inner = ReadMathGroup(tex, i);
                if (!inner.empty()) EmitRun(result, inner, flags | InlineCodeFlag, linkUrl);
            } else if (cmd == L"mathbb") {
                const View inner = ReadMathGroup(tex, i);
                for (wchar_t c : inner) {
                    if (c == L'R') EmitRun(result, L"\u211D", flags, linkUrl);
                    else if (c == L'N') EmitRun(result, L"\u2115", flags, linkUrl);
                    else if (c == L'Z') EmitRun(result, L"\u2124", flags, linkUrl);
                    else if (c == L'Q') EmitRun(result, L"\u211A", flags, linkUrl);
                    else if (c == L'C') EmitRun(result, L"\u2102", flags, linkUrl);
                    else if (c == L'P') EmitRun(result, L"\u2119", flags, linkUrl);
                    else if (c == L'E') EmitRun(result, L"\U0001D53C", flags, linkUrl);
                    else EmitRun(result, View(&c, 1), flags, linkUrl);
                }
            } else if (cmd == L"frac" || cmd == L"dfrac" || cmd == L"tfrac") {
                const View num = ReadMathGroup(tex, i);
                const View den = ReadMathGroup(tex, i);
                const bool pNum = NeedsMathFractionParens(num);
                const bool pDen = NeedsMathFractionParens(den);
                if (pNum) EmitRun(result, L"(", flags, linkUrl);
                AppendMathTex(result, Trim(num), flags, linkUrl, false, depth + 1);
                if (pNum) EmitRun(result, L")", flags, linkUrl);
                EmitRun(result, L"/", flags, linkUrl);
                if (pDen) EmitRun(result, L"(", flags, linkUrl);
                AppendMathTex(result, Trim(den), flags, linkUrl, false, depth + 1);
                if (pDen) EmitRun(result, L")", flags, linkUrl);
            } else if (cmd == L"sqrt") {
                View degree;
                if (i < tex.size() && tex[i] == L'[') {
                    const size_t closeBr = tex.find(L']', i + 1);
                    if (closeBr != View::npos) {
                        degree = tex.substr(i + 1, closeBr - (i + 1));
                        i = closeBr + 1;
                    }
                }
                const View body = ReadMathGroup(tex, i);
                if (!degree.empty()) AppendMathTex(result, degree, (flags & ~ItalicFlag) | SupFlag, linkUrl, false, depth + 1);
                EmitRun(result, L"\u221A(", flags, linkUrl);
                AppendMathTex(result, Trim(body), flags, linkUrl, false, depth + 1);
                EmitRun(result, L")", flags, linkUrl);
            } else if (cmd == L"bar" || cmd == L"overline" || cmd == L"hat" || cmd == L"vec" || cmd == L"tilde") {
                const View body = ReadMathGroup(tex, i);
                const View trimmed = Trim(body);
                AppendMathTex(result, trimmed, flags, linkUrl, false, depth + 1);
                if (trimmed.size() == 1) {
                    const wchar_t mark = cmd == L"hat" ? L'\u0302' : cmd == L"vec" ? L'\u20D7'
                        : cmd == L"tilde" ? L'\u0303' : L'\u0304';
                    EmitRun(result, View(&mark, 1), flags, linkUrl);
                }
            } else if (const auto sym = LookupMathSymbol(cmd)) {
                EmitRun(result, *sym, flags, linkUrl);
            } else if (IsMathFunctionName(cmd)) {
                EmitRun(result, cmd, flags & ~ItalicFlag, linkUrl);
            } else {
                EmitRun(result, L"\\", flags, linkUrl);
                EmitRun(result, cmd, flags, linkUrl);
            }
            continue;
        }
        if (MathAsciiLetter(ch)) {
            size_t runEnd = i + 1;
            while (runEnd < tex.size() && MathAsciiLetter(tex[runEnd])) ++runEnd;
            const bool singleVar = !plainText && (runEnd - i == 1) && !(flags & (SubFlag | SupFlag | BoldFlag));
            EmitRun(result, tex.substr(i, runEnd - i), singleVar ? (flags | ItalicFlag) : flags, linkUrl);
            i = runEnd;
            continue;
        }
        size_t next = i + 1;
        while (next < tex.size()) {
            const wchar_t c = tex[next];
            if (Space(c) || c == L'~' || c == L'&' || c == L'{' || c == L'}' ||
                c == L'_' || c == L'^' || c == L'\\' || MathAsciiLetter(c))
                break;
            ++next;
        }
        EmitRun(result, tex.substr(i, next - i), flags, linkUrl);
        i = next;
    }
}

std::optional<size_t> MatchInlineMath(View text, size_t dollarPos) {
    if (dollarPos + 2 >= text.size()) return {};
    if (dollarPos > 0 && AsciiWordChar(text[dollarPos - 1])) return {};
    const wchar_t next = text[dollarPos + 1];
    if (Space(next) || next == L'$' || next == L'{') return {};
    for (size_t i = dollarPos + 2; i < text.size(); ++i) {
        if (text[i] == L'\n' || text[i] == L'`') return {};
        if (text[i] == L'\\' && i + 1 < text.size()) {
            ++i;
            continue;
        }
        if (text[i] == L'$') {
            if (Space(text[i - 1])) return {};
            if (i + 1 < text.size() &&
                (AsciiWordChar(text[i + 1]) || text[i + 1] == L'$' || text[i + 1] == L'{' || text[i + 1] == L'('))
                return {};
            return i;
        }
    }
    return {};
}

bool AllowedExternalScheme(View url) {
    return StartsNoCase(url, L"http://") || StartsNoCase(url, L"https://") || StartsNoCase(url, L"mailto:");
}

void AppendInline(Text &result, View text, unsigned flags = 0, View linkUrl = {}, int depth = 0) {
    if (depth >= MaxInlineDepth) {
        EmitPlainWithEntitiesAndBr(result, text, flags, linkUrl);
        return;
    }
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'\\' && i + 1 < text.size() && AsciiPunctuation(text[i + 1])) {
            EmitRun(result, text.substr(i + 1, 1), flags, linkUrl);
            i += 2;
        } else if (text[i] == L'`') {
            const auto ticks = Run(text, i, L'`');
            const auto end = ClosingTicks(text, i + ticks, ticks);
            if (end != View::npos) {
                View inner = text.substr(i + ticks, end - (i + ticks));
                if (inner.size() >= 2 && inner.front() == L' ' && inner.back() == L' ' &&
                    inner.find_first_not_of(L' ') != View::npos) {
                    inner.remove_prefix(1);
                    inner.remove_suffix(1);
                }
                EmitRun(result, inner, flags | InlineCodeFlag, linkUrl);
                i = end + ticks;
            } else {
                EmitRun(result, text.substr(i, ticks), flags, linkUrl);
                i += ticks;
            }
        } else if (text[i] == L'<') {
            if (const size_t brLen = MatchBrTag(text.substr(i))) {
                EmitRun(result, L"\n", flags, linkUrl);
                i += brLen;
            } else if (const auto autoLink = MatchAutolink(text.substr(i))) {
                const View full = text.substr(i + 1, autoLink->second - 2);
                EmitRun(result, full, flags | LinkFlag, autoLink->first);
                i += autoLink->second;
            } else if (const auto html = MatchPairedHtmlTag(text.substr(i))) {
                if (html->addedFlag == InlineCodeFlag) {
                    EmitPlainWithEntitiesAndBr(result, html->inner, flags | InlineCodeFlag, linkUrl);
                } else {
                    const View nextUrl = !html->linkUrl.empty() ? View(html->linkUrl) : linkUrl;
                    AppendInline(result, html->inner, flags | html->addedFlag, nextUrl, depth + 1);
                }
                i += html->consumed;
            } else {
                EmitRun(result, text.substr(i, 1), flags, linkUrl);
                ++i;
            }
        } else if (text[i] == L'!' && i + 1 < text.size() && text[i + 1] == L'[') {
            if (const auto img = MatchLinkOrImage(text, i + 1)) {
                Text alt;
                AppendInline(alt, img->label, 0, {}, depth + 1);
                const std::wstring caption = L"[图片: " + (alt.value.empty() ? L"图像" : alt.value) + L"]";
                const unsigned imgFlags = flags | MutedFlag | (AllowedExternalScheme(img->url) ? LinkFlag : 0u);
                const View nextUrl = AllowedExternalScheme(img->url) ? View(img->url) : linkUrl;
                EmitRun(result, caption, imgFlags, nextUrl);
                i = img->consumed;
            } else {
                EmitRun(result, text.substr(i, 1), flags, linkUrl);
                ++i;
            }
        } else if (text[i] == L'[') {
            if (const auto link = MatchLinkOrImage(text, i)) {
                AppendInline(result, link->label, flags | LinkFlag, link->url, depth + 1);
                i = link->consumed;
            } else {
                EmitRun(result, text.substr(i, 1), flags, linkUrl);
                ++i;
            }
        } else if (Starts(text.substr(i), L"~~") && i + 2 < text.size() && !Space(text[i + 2])) {
            if (const auto end = FindClosingTildes(text, i + 2); end != View::npos) {
                AppendInline(result, text.substr(i + 2, end - (i + 2)), flags | StrikeFlag, linkUrl, depth + 1);
                i = end + 2;
            } else {
                EmitRun(result, L"~~", flags, linkUrl);
                i += 2;
            }
        } else if (text[i] == L'*' || text[i] == L'_') {
            const wchar_t marker = text[i];
            const bool underscore = marker == L'_';
            const size_t run = std::min<size_t>(3, Run(text, i, marker));
            const bool canOpen = !underscore || i == 0 || !AsciiWordChar(text[i - 1]);
            bool matched = false;
            if (canOpen) {
                for (size_t tryLen = run; tryLen >= 1; --tryLen) {
                    if (i + tryLen >= text.size() || Space(text[i + tryLen])) continue;
                    const size_t end = FindClosingEmphasis(text, i + tryLen, marker, tryLen, underscore);
                    if (end != View::npos && end > i + tryLen) {
                        const unsigned added = tryLen == 3 ? (BoldFlag | ItalicFlag)
                            : tryLen == 2 ? BoldFlag : ItalicFlag;
                        AppendInline(result, text.substr(i + tryLen, end - (i + tryLen)),
                                     flags | added, linkUrl, depth + 1);
                        i = end + tryLen;
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                EmitRun(result, text.substr(i, run), flags, linkUrl);
                i += run;
            }
        } else if (text[i] == L'$') {
            if (const auto close = MatchInlineMath(text, i)) {
                AppendMathTex(result, text.substr(i + 1, *close - (i + 1)), flags | MathFlag, linkUrl);
                i = *close + 1;
            } else {
                EmitRun(result, text.substr(i, 1), flags, linkUrl);
                ++i;
            }
        } else if (text[i] == L'&') {
            if (const auto ent = DecodeEntity(text.substr(i))) {
                EmitRun(result, ent->first, flags, linkUrl);
                i += ent->second;
            } else {
                EmitRun(result, text.substr(i, 1), flags, linkUrl);
                ++i;
            }
        } else {
            size_t next = i + 1;
            while (next < text.size()) {
                const wchar_t c = text[next];
                if (c == L'\\' || c == L'`' || c == L'<' || c == L'[' || c == L'!' ||
                    c == L'~' || c == L'*' || c == L'_' || c == L'$' || c == L'&')
                    break;
                ++next;
            }
            EmitRun(result, text.substr(i, next - i), flags, linkUrl);
            i = next;
        }
    }
}

Text Inline(View line) {
    Text result;
    AppendInline(result, line);
    return result;
}

std::vector<View> SplitTopLevelBr(View line) {
    std::vector<View> parts;
    size_t start = 0;
    for (size_t i = 0; i < line.size();) {
        if (line[i] == L'\\' && i + 1 < line.size()) {
            i += 2;
            continue;
        }
        if (line[i] == L'`') {
            const auto ticks = Run(line, i, L'`');
            const auto end = ClosingTicks(line, i + ticks, ticks);
            i = end == View::npos ? i + ticks : end + ticks;
            continue;
        }
        if (line[i] == L'<') {
            if (const size_t brLen = MatchBrTag(line.substr(i))) {
                parts.push_back(line.substr(start, i - start));
                i += brLen;
                start = i;
                continue;
            }
        }
        ++i;
    }
    if (start < line.size() || parts.empty()) parts.push_back(line.substr(start));
    return parts;
}

std::vector<View> Cells(View line) {
    line = Trim(line);
    if (line.empty()) return {};
    std::vector<View> result;
    size_t start = line.front() == L'|' ? 1 : 0;
    bool divided = start != 0;
    for (size_t i = start; i < line.size(); ++i) {
        if (line[i] == L'`' && !Escaped(line, i)) {
            const auto ticks = Run(line, i, L'`');
            const auto end = ClosingTicks(line, i + ticks, ticks);
            i = end == View::npos ? i + ticks - 1 : end + ticks - 1;
        } else if (line[i] == L'|' && !Escaped(line, i)) {
            result.push_back(Trim(line.substr(start, i - start)));
            start = i + 1;
            divided = true;
        }
    }
    if (!divided) return {};
    if (start < line.size()) result.push_back(Trim(line.substr(start)));
    return result;
}

std::vector<Alignment> Divider(View line) {
    std::vector<Alignment> alignment;
    for (auto cell : Cells(line)) {
        const bool left = !cell.empty() && cell.front() == L':';
        if (left) cell.remove_prefix(1);
        const bool right = !cell.empty() && cell.back() == L':';
        if (right) cell.remove_suffix(1);
        if (cell.size() < 3 || cell.find_first_not_of(L'-') != View::npos) return {};
        alignment.push_back(right ? (left ? Alignment::Center : Alignment::Right) : Alignment::Left);
    }
    return alignment;
}

struct SourceLine { View text; size_t begin, next; bool ended; };
SourceLine Line(View source, size_t offset) {
    const auto lf = source.find(L'\n', offset);
    auto text = source.substr(offset, lf == View::npos ? lf : lf - offset);
    if (!text.empty() && text.back() == L'\r') text.remove_suffix(1);
    return {text, offset, lf == View::npos ? source.size() : lf + 1, lf != View::npos};
}

struct Fence { wchar_t marker; size_t length; View tag; };
std::optional<Fence> FenceAt(View text) {
    const auto leading = text.find_first_not_of(L' ');
    if (leading == View::npos || leading > 3) return {};
    text.remove_prefix(leading);
    const auto marker = text.front();
    if (marker != L'`' && marker != L'~') return {};
    const auto length = Run(text, 0, marker);
    if (length < 3) return {};
    const auto tag = Trim(text.substr(length));
    if (marker == L'`' && tag.find(L'`') != View::npos) return {};
    return Fence{marker, length, tag};
}

bool HorizontalRule(View line) {
    line = Trim(line);
    if (line.size() < 3 || View(L"-*_").find(line.front()) == View::npos) return false;
    const auto mark = line.front();
    return std::count(line.begin(), line.end(), mark) >= 3 &&
        std::all_of(line.begin(), line.end(), [mark](wchar_t c) { return Space(c) || c == mark; });
}

struct ListMatch {
    size_t indentCols = 0;
    bool ordered = false;
    bool isTask = false;
    bool checked = false;
    View prefix; // e.g. "1. "
    View content;
};

std::optional<ListMatch> MatchListLine(View text) {
    size_t pos = 0;
    const size_t indentCols = LeadingIndentCols(text, &pos);
    const View rest = text.substr(pos);
    if (rest.size() >= 2 && View(L"-*+").find(rest.front()) != View::npos && Space(rest[1])) {
        if (HorizontalRule(text)) return {};
        View after = rest.substr(2);
        while (!after.empty() && Space(after.front())) after.remove_prefix(1);
        if (after.size() >= 3 && after[0] == L'[' &&
            (after[1] == L' ' || after[1] == L'x' || after[1] == L'X') && after[2] == L']' &&
            (after.size() == 3 || Space(after[3]))) {
            const bool checked = after[1] != L' ';
            View body = after.size() > 3 ? after.substr(4) : View{};
            while (!body.empty() && Space(body.front())) body.remove_prefix(1);
            return ListMatch{indentCols, false, true, checked, {}, body};
        }
        return ListMatch{indentCols, false, false, false, {}, after};
    }
    const size_t digits = rest.find_first_not_of(L"0123456789");
    if (digits != View::npos && digits > 0 && digits <= 9 && digits + 1 < rest.size() &&
        (rest[digits] == L'.' || rest[digits] == L')') && Space(rest[digits + 1])) {
        return ListMatch{indentCols, true, false, false, rest.substr(0, digits + 2), Trim(rest.substr(digits + 2))};
    }
    return {};
}

std::optional<std::pair<int, View>> MatchHeadingLine(View text) {
    const View trimmed = Trim(text);
    const size_t hashes = Run(trimmed, 0, L'#');
    if (!hashes || hashes > 6) return {};
    if (hashes < trimmed.size() && !Space(trimmed[hashes])) return {};
    View body = Trim(trimmed.substr(hashes));
    while (!body.empty() && body.back() == L'#') {
        size_t pos = body.size();
        while (pos > 0 && body[pos - 1] == L'#') --pos;
        if (pos == 0 || Space(body[pos - 1])) {
            body = Trim(body.substr(0, pos));
            break;
        }
        break;
    }
    return std::make_pair(static_cast<int>(hashes), body);
}

AlertKind MatchAlertTag(View &text) {
    View t = Trim(text);
    if (!Starts(t, L"[!")) return AlertKind::None;
    const size_t close = t.find(L']', 2);
    if (close == View::npos) return AlertKind::None;
    const View tag = t.substr(2, close - 2);
    AlertKind kind = AlertKind::None;
    if (EqualsNoCase(tag, L"NOTE")) kind = AlertKind::Note;
    else if (EqualsNoCase(tag, L"TIP")) kind = AlertKind::Tip;
    else if (EqualsNoCase(tag, L"IMPORTANT")) kind = AlertKind::Important;
    else if (EqualsNoCase(tag, L"WARNING")) kind = AlertKind::Warning;
    else if (EqualsNoCase(tag, L"CAUTION")) kind = AlertKind::Caution;
    if (kind == AlertKind::None) return AlertKind::None;
    text = Trim(t.substr(close + 1));
    return kind;
}

std::pair<int, View> StripQuoteMarkers(View line) {
    View t = line;
    int depth = 0;
    while (depth < 6) {
        size_t spaces = 0;
        while (spaces < 3 && spaces < t.size() && t[spaces] == L' ') ++spaces;
        if (spaces >= t.size() || t[spaces] != L'>') break;
        t.remove_prefix(spaces + 1);
        if (!t.empty() && Space(t.front())) t.remove_prefix(1);
        ++depth;
    }
    return {depth, t};
}

enum class Language { Plain, JavaScript, TypeScript, Python, Java, CSharp, Cpp, Bash, Sql, Html, Css, Json, Yaml, Ini, Diff, Make, Dts };

Language LanguageFor(View tag) {
    const auto name = Lower(tag);
    if (name == L"js" || name == L"javascript" || name == L"jsx" || name == L"mjs") return Language::JavaScript;
    if (name == L"ts" || name == L"typescript" || name == L"tsx") return Language::TypeScript;
    if (name == L"py" || name == L"python" || name == L"python3") return Language::Python;
    if (name == L"java") return Language::Java;
    if (name == L"cs" || name == L"csharp" || name == L"c#") return Language::CSharp;
    if (name == L"c" || name == L"c++" || name == L"cpp" || name == L"cxx" || name == L"h") return Language::Cpp;
    if (name == L"bash" || name == L"sh" || name == L"shell" || name == L"zsh" || name == L"shellscript" ||
        name == L"cmd" || name == L"bat" || name == L"powershell" || name == L"ps1" || name == L"pwsh")
        return Language::Bash;
    if (name == L"sql" || name == L"mysql" || name == L"postgresql" || name == L"sqlite" || name == L"tsql") return Language::Sql;
    if (name == L"html" || name == L"xml" || name == L"svg") return Language::Html;
    if (name == L"css" || name == L"scss") return Language::Css;
    if (name == L"json" || name == L"jsonc" || name == L"json5") return Language::Json;
    if (name == L"yaml" || name == L"yml") return Language::Yaml;
    if (name == L"ini" || name == L"toml" || name == L"conf" || name == L"cfg") return Language::Ini;
    if (name == L"diff" || name == L"patch") return Language::Diff;
    if (name == L"makefile" || name == L"make" || name == L"mk" || name == L"cmake") return Language::Make;
    if (name == L"dts" || name == L"dtsi" || name == L"devicetree") return Language::Dts;
    return Language::Plain;
}

bool Word(wchar_t ch) { return ch == L'_' || ch == L'$' || std::iswalnum(ch); }

bool KeywordIn(View words, View word) {
    for (size_t pos = words.find(word); pos != View::npos; pos = words.find(word, pos + 1)) {
        const auto end = pos + word.size();
        if ((pos == 0 || words[pos - 1] == L' ') && (end == words.size() || words[end] == L' ')) return true;
    }
    return false;
}

bool Keyword(Language language, View word) {
    using L = Language;
    constexpr View script = L"as async await break case catch class const continue debugger default delete do else export extends false finally for from function if import in instanceof let new null of return static super switch this throw true try typeof undefined var void while with yield";
    constexpr View control = L"break case catch class const continue default do else enum false finally for if new null private protected public return static super switch this throw true try void while";
    switch (language) {
    case L::JavaScript: return KeywordIn(script, word);
    case L::TypeScript: return KeywordIn(script, word) || KeywordIn(L"abstract any asserts boolean declare enum implements infer interface is keyof namespace never number override private protected public readonly satisfies string symbol type unknown", word);
    case L::Python: return KeywordIn(L"False None True and as assert async await break case class continue def del elif else except finally for from global if import in is lambda match nonlocal not or pass raise return try while with yield", word);
    case L::Java: return KeywordIn(control, word) || KeywordIn(L"abstract boolean byte char double exports final float implements import int interface long module native package permits record sealed short strictfp synchronized throws transient var volatile", word);
    case L::CSharp: return KeywordIn(control, word) || KeywordIn(L"abstract async await bool byte char decimal delegate double event foreach get int interface internal lock namespace out override readonly record ref required sbyte sealed set short string struct uint ulong unsafe ushort using var virtual yield", word);
    case L::Cpp: return KeywordIn(control, word) || KeywordIn(L"alignas auto bool char constexpr consteval constinit decltype double extern float inline int long namespace noexcept nullptr operator requires short signed sizeof struct template typedef typename union unsigned using virtual volatile wchar_t", word);
    case L::Bash: return KeywordIn(L"case do done elif else esac fi for function if in select then time until while alias call cd cat chmod chown cl clang clang++ cmake cp ctest curl declare dir echo env eval exec exit export find g++ gcc git grep head ls make mkdir msbuild mv ninja printf python python3 read return rm scp sed set sftp shift source ssh sudo tail tee test touch trap true false unset wait where which xargs", word);
    case L::Sql: return KeywordIn(L"add all alter and as asc begin between by case check column commit create cross database default delete desc distinct drop else end exists false foreign from full group having in index inner insert into is join left like limit not null offset on or order outer primary references returning right rollback select set table then true union unique update values view when where with", Lower(word));
    case L::Json: return word == L"true" || word == L"false" || word == L"null";
    case L::Yaml:
    case L::Ini:
        return word == L"true" || word == L"false" || word == L"null" || word == L"yes" || word == L"no";
    case L::Make: return KeywordIn(L"ifeq ifneq ifdef ifndef else endif include export define endef target project add_executable add_library set option message if elseif", Lower(word));
    case L::Dts: return KeywordIn(L"okay disabled", word);
    default: return false;
    }
}

class Highlighter {
  public:
    explicit Highlighter(Language language) : language_(language) {}
    Text Highlight(View line) {
        Text result{std::wstring(line), {}, {}};
        using L = Language;
        if (language_ == L::Plain) return result;
        auto add = [&](size_t start, size_t end, Ink ink) {
            if (end > start) result.spans.push_back({start, end - start, ink});
        };
        if (language_ == L::Diff) {
            if (Starts(line, L"+++") || Starts(line, L"---") || Starts(line, L"@@")) add(0, line.size(), Ink::Name);
            else if (Starts(line, L"+")) add(0, line.size(), Ink::String);
            else if (Starts(line, L"-")) add(0, line.size(), Ink::Number);
            else if (Starts(line, L"diff ") || Starts(line, L"index ")) add(0, line.size(), Ink::Keyword);
            return result;
        }
        const bool cFamily = language_ == L::Cpp || language_ == L::CSharp || language_ == L::Java ||
            language_ == L::JavaScript || language_ == L::TypeScript || language_ == L::Json || language_ == L::Dts;
        for (size_t i = 0; i < line.size();) {
            if (!terminator_.empty()) {
                const auto close = line.find(terminator_, i);
                if (close == View::npos) {
                    add(i, line.size(), continuation_);
                    break;
                }
                add(i, close + terminator_.size(), continuation_);
                i = close + terminator_.size();
                terminator_.clear();
                quote_ = false;
                continue;
            }
            const auto ch = line[i];
            if ((cFamily || language_ == L::Css || language_ == L::Sql) &&
                ((cFamily && Starts(line.substr(i), L"//")) || (language_ == L::Sql && Starts(line.substr(i), L"--")))) {
                add(i, line.size(), Ink::Comment);
                break;
            }
            if ((language_ == L::Python || language_ == L::Bash || language_ == L::Yaml || language_ == L::Ini ||
                 language_ == L::Make) && ch == L'#') {
                add(i, line.size(), Ink::Comment);
                break;
            }
            if (language_ == L::Ini && ch == L';') {
                add(i, line.size(), Ink::Comment);
                break;
            }
            if ((cFamily || language_ == L::Css || language_ == L::Sql) && Starts(line.substr(i), L"/*")) {
                const auto close = line.find(L"*/", i + 2);
                if (close == View::npos) {
                    add(i, line.size(), Ink::Comment);
                    terminator_ = L"*/";
                    continuation_ = Ink::Comment;
                    break;
                }
                add(i, close + 2, Ink::Comment);
                i = close + 2;
                continue;
            }
            if (language_ == L::Html && Starts(line.substr(i), L"<!--")) {
                const auto close = line.find(L"-->", i + 4);
                if (close == View::npos) {
                    add(i, line.size(), Ink::Comment);
                    terminator_ = L"-->";
                    continuation_ = Ink::Comment;
                    break;
                }
                add(i, close + 3, Ink::Comment);
                i = close + 3;
                continue;
            }
            if (language_ == L::Ini && ch == L'[' && Trim(line.substr(0, i)).empty()) {
                const auto close = line.find(L']', i + 1);
                if (close != View::npos) {
                    add(i, close + 1, Ink::Name);
                    i = close + 1;
                    continue;
                }
            }
            size_t open = 0;
            View close;
            Ink ink = Ink::String;
            bool quote = false;
            if (language_ == L::Python && (Starts(line.substr(i), L"\"\"\"") || Starts(line.substr(i), L"'''"))) {
                open = 3;
                close = line.substr(i, 3);
                quote = true;
            } else if (language_ == L::JavaScript && ch == L'`') {
                open = 1;
                close = L"`";
                quote = true;
            } else if (language_ == L::CSharp && Starts(line.substr(i), L"@\"")) {
                open = 2;
                close = L"\"";
                quote = true;
            }
            if (open) {
                add(i, i + open, ink);
                terminator_ = std::wstring(close);
                continuation_ = ink;
                quote_ = quote;
                i += open;
                continue;
            }
            if (language_ == L::Html && (ch == L'<' || (tag_ && ch == L'>'))) {
                tag_ = ch == L'<';
                size_t end = i + 1;
                if (tag_ && end < line.size() && (line[end] == L'/' || line[end] == L'!')) ++end;
                while (tag_ && end < line.size() && (Word(line[end]) || line[end] == L'-' || line[end] == L':')) ++end;
                add(i, end, Ink::Keyword);
                i = end;
                continue;
            }
            if (language_ == L::Html && !tag_) { ++i; continue; }
            if (ch == L'"' || ch == L'\'') {
                size_t end = i + 1;
                while (end < line.size()) {
                    if (line[end] == L'\\' && end + 1 < line.size() && !(language_ == L::Bash && ch == L'\'')) end += 2;
                    else if (line[end++] == ch) {
                        if (language_ == L::Sql && end < line.size() && line[end] == ch) ++end;
                        else break;
                    }
                }
                const auto after = line.find_first_not_of(L" \t", end);
                const bool isKey = (language_ == L::Json || language_ == L::Yaml) &&
                                   after != View::npos && line[after] == L':';
                add(i, end, isKey ? Ink::Name : Ink::String);
                i = end;
                continue;
            }
            if ((language_ == L::Cpp || language_ == L::Dts) && (ch == L'#' || (language_ == L::Dts && ch == L'/')) &&
                Trim(line.substr(0, i)).empty() && (ch == L'#' || Starts(line.substr(i), L"/dts-v1/"))) {
                add(i, line.size(), Ink::Name);
                break;
            }
            if ((language_ == L::Bash || language_ == L::Make) && ch == L'$') {
                size_t end = i + 1;
                if (end < line.size() && (line[end] == L'{' || line[end] == L'(')) {
                    const wchar_t closeCh = line[end] == L'{' ? L'}' : L')';
                    const auto closeBrace = line.find(closeCh, end + 1);
                    end = closeBrace == View::npos ? end + 1 : closeBrace + 1;
                } else {
                    while (end < line.size() && Word(line[end])) ++end;
                    if (end == i + 1 && end < line.size() && View(L"?#@*!-").find(line[end]) != View::npos) ++end;
                }
                add(i, end, Ink::Name);
                i = end;
                continue;
            }
            if (language_ == L::Bash && ch == L'-' && (i == 0 || line[i - 1] == L' ' || line[i - 1] == L'\t') &&
                i + 1 < line.size() && (line[i + 1] == L'-' || std::iswalpha(line[i + 1]))) {
                size_t end = i + 1;
                while (end < line.size() && (Word(line[end]) || line[end] == L'-')) ++end;
                add(i, end, Ink::Name);
                i = end;
                continue;
            }
            if (std::iswdigit(ch) || (ch == L'-' && language_ == L::Json && i + 1 < line.size() && std::iswdigit(line[i + 1]))) {
                size_t end = i + 1;
                while (end < line.size() && (Word(line[end]) || line[end] == L'.' ||
                    ((line[end] == L'-' || line[end] == L'+') && (line[end - 1] == L'e' || line[end - 1] == L'E')))) ++end;
                add(i, end, Ink::Number);
                i = end;
                continue;
            }
            if (Word(ch) || ((language_ == L::Css || language_ == L::Yaml || language_ == L::Dts) &&
                             (ch == L'-' || ch == L'@' || ch == L'#'))) {
                size_t end = i + 1;
                while (end < line.size() && (Word(line[end]) || ((language_ == L::Css || language_ == L::Html ||
                    language_ == L::Yaml || language_ == L::Dts) && line[end] == L'-'))) ++end;
                const auto after = line.find_first_not_of(L" \t", end);
                if (Keyword(language_, line.substr(i, end - i))) add(i, end, Ink::Keyword);
                else if (language_ == L::Html ||
                    (language_ == L::Css && (ch == L'@' || ch == L'#' || (after != View::npos && line[after] == L':'))) ||
                    (language_ == L::Yaml && after != View::npos && line[after] == L':') ||
                    ((language_ == L::Ini || language_ == L::Dts) && after != View::npos &&
                     (line[after] == L'=' || line[after] == L';')))
                    add(i, end, Ink::Name);
                i = end;
                continue;
            }
            ++i;
        }
        return result;
    }
  private:
    Language language_;
    std::wstring terminator_;
    Ink continuation_ = Ink::String;
    bool quote_ = false, tag_ = false;
};

std::wstring CollapseHtmlWhitespace(View raw) {
    std::wstring out;
    out.reserve(raw.size());
    bool pendingSpace = false;
    auto isWs = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == L'<') {
            if (const size_t brLen = MatchBrTag(raw.substr(i))) {
                pendingSpace = false;
                out.append(raw.substr(i, brLen));
                i += brLen;
                while (i < raw.size() && isWs(raw[i])) ++i;
                continue;
            }
        }
        if (isWs(raw[i])) {
            if (!out.empty()) pendingSpace = true;
            ++i;
        } else {
            if (pendingSpace) {
                out.push_back(L' ');
                pendingSpace = false;
            }
            out.push_back(raw[i]);
            ++i;
        }
    }
    return out;
}

std::optional<size_t> FindClosingTagNoCase(View text, size_t start, View closeTag) {
    for (size_t i = start; i + closeTag.size() <= text.size(); ++i) {
        if (text[i] == L'<' && StartsNoCase(text.substr(i), closeTag)) return i;
    }
    return {};
}

bool TryParseHtmlTable(View source, size_t &offset, size_t begin, Block &block) {
    const size_t scanLimit = std::min(source.size(), begin + 262144);
    const auto tableEnd = FindClosingTagNoCase(source.substr(0, scanLimit), begin, L"</table>");
    if (!tableEnd) return false;
    size_t closeGt = source.find(L'>', *tableEnd);
    if (closeGt == View::npos || closeGt >= scanLimit) return false;
    const View html = source.substr(begin, closeGt + 1 - begin);

    std::vector<std::vector<std::pair<Alignment, std::wstring>>> rawRows;
    size_t maxCols = 0;
    for (size_t pos = 0; pos < html.size();) {
        if (html[pos] != L'<' || !StartsNoCase(html.substr(pos), L"<tr")) {
            ++pos;
            continue;
        }
        const size_t afterTr = pos + 3;
        if (afterTr >= html.size() || (!Space(html[afterTr]) && html[afterTr] != L'>')) {
            ++pos;
            continue;
        }
        const size_t trGt = html.find(L'>', afterTr);
        if (trGt == View::npos) break;
        const auto trEnd = FindClosingTagNoCase(html, trGt + 1, L"</tr>");
        if (!trEnd) break;
        const View rowHtml = html.substr(trGt + 1, *trEnd - (trGt + 1));
        std::vector<std::pair<Alignment, std::wstring>> rowCells;
        for (size_t cPos = 0; cPos < rowHtml.size();) {
            if (rowHtml[cPos] != L'<') { ++cPos; continue; }
            const bool isTh = StartsNoCase(rowHtml.substr(cPos), L"<th");
            const bool isTd = !isTh && StartsNoCase(rowHtml.substr(cPos), L"<td");
            if (!isTh && !isTd) { ++cPos; continue; }
            const size_t afterCell = cPos + 3;
            if (afterCell >= rowHtml.size() || (!Space(rowHtml[afterCell]) && rowHtml[afterCell] != L'>')) {
                ++cPos;
                continue;
            }
            const size_t cellGt = rowHtml.find(L'>', afterCell);
            if (cellGt == View::npos) break;
            const auto cellEnd = FindClosingTagNoCase(rowHtml, cellGt + 1, isTh ? L"</th>" : L"</td>");
            if (!cellEnd) break;
            Alignment align = Alignment::Left;
            const View attrs = rowHtml.substr(afterCell, cellGt - afterCell);
            if (const auto alignAttr = ExtractHtmlAttr(attrs, L"align")) {
                const auto low = Lower(*alignAttr);
                if (low == L"center") align = Alignment::Center;
                else if (low == L"right") align = Alignment::Right;
            } else if (const auto styleAttr = ExtractHtmlAttr(attrs, L"style")) {
                const auto low = Lower(*styleAttr);
                if (low.find(L"text-align") != std::wstring::npos) {
                    if (low.find(L"center") != std::wstring::npos) align = Alignment::Center;
                    else if (low.find(L"right") != std::wstring::npos) align = Alignment::Right;
                }
            }
            rowCells.emplace_back(align, CollapseHtmlWhitespace(rowHtml.substr(cellGt + 1, *cellEnd - (cellGt + 1))));
            cPos = *cellEnd + 5;
        }
        if (!rowCells.empty()) {
            maxCols = std::max(maxCols, rowCells.size());
            rawRows.push_back(std::move(rowCells));
        }
        pos = *trEnd + 5;
    }
    if (rawRows.empty() || maxCols == 0 || maxCols > MaxTableCells) return false;
    block.kind = Kind::Table;
    block.alignment.assign(maxCols, Alignment::Left);
    for (size_t c = 0; c < rawRows.front().size(); ++c) block.alignment[c] = rawRows.front()[c].first;
    for (const auto &rawRow : rawRows) {
        if (block.rows.size() >= MaxTableCells / maxCols) break;
        auto &cells = block.rows.emplace_back();
        for (size_t c = 0; c < maxCols; ++c)
            cells.push_back(c < rawRow.size() ? Inline(rawRow[c].second) : Text{});
    }
    size_t next = closeGt + 1;
    while (next < source.size() && (source[next] == L' ' || source[next] == L'\t' || source[next] == L'\r')) ++next;
    if (next < source.size() && source[next] == L'\n') ++next;
    offset = next;
    return true;
}

std::vector<Block> ParseInternal(View source, bool proseMarkup, int depth, bool inlineFormatted = false);

bool TryParseDetailsBlock(View source, size_t &offset, size_t begin, Block &block, int depth) {
    if (depth >= MaxParseDepth) return false;
    const auto firstLine = Line(source, begin);
    const View trimmed = Trim(firstLine.text);
    if (!StartsNoCase(trimmed, L"<details")) return false;
    if (trimmed.size() > 8 && !Space(trimmed[8]) && trimmed[8] != L'>') return false;
    const size_t openGt = trimmed.find(L'>');
    if (openGt == View::npos) return false;
    const View attrs = trimmed.substr(8, openGt - 8);
    block.detailsOpen = Lower(attrs).find(L"open") != std::wstring::npos;

    size_t pos = firstLine.next;
    std::wstring bodyText;
    View trailing = trimmed.substr(openGt + 1);
    if (!trailing.empty()) {
        if (const auto sameLineEnd = FindClosingTagNoCase(trailing, 0, L"</details>")) {
            bodyText.append(trailing.substr(0, *sameLineEnd));
            pos = firstLine.next;
            goto parsed_bounds;
        }
        bodyText.append(trailing);
        bodyText.push_back(L'\n');
    }
    while (pos < source.size()) {
        const auto cur = Line(source, pos);
        pos = cur.next;
        if (const auto closePos = FindClosingTagNoCase(cur.text, 0, L"</details>")) {
            bodyText.append(cur.text.substr(0, *closePos));
            break;
        }
        bodyText.append(cur.text);
        if (cur.ended) bodyText.push_back(L'\n');
    }
parsed_bounds:
    View inner = bodyText;
    std::wstring summaryText = L"详细信息";
    if (const auto sumStart = FindClosingTagNoCase(inner, 0, L"<summary")) {
        const size_t sumAfter = *sumStart + 8;
        if (sumAfter < inner.size() && (Space(inner[sumAfter]) || inner[sumAfter] == L'>')) {
            const size_t sumGt = inner.find(L'>', sumAfter);
            if (sumGt != View::npos) {
                if (const auto sumEnd = FindClosingTagNoCase(inner, sumGt + 1, L"</summary>")) {
                    const auto extracted = CollapseHtmlWhitespace(inner.substr(sumGt + 1, *sumEnd - (sumGt + 1)));
                    if (!extracted.empty()) summaryText = extracted;
                    std::wstring remainder;
                    remainder.append(inner.substr(0, *sumStart));
                    remainder.append(inner.substr(*sumEnd + 10));
                    bodyText = std::move(remainder);
                }
            }
        }
    }
    block.kind = Kind::Details;
    block.summary = Inline(summaryText);
    const View trimmedBody = TrimAllSpace(bodyText);
    if (!trimmedBody.empty()) block.children = ParseInternal(trimmedBody, true, depth + 1);
    offset = pos;
    return true;
}

bool TryParseMathBlock(View source, size_t &offset, size_t begin, Block &block) {
    const auto first = Line(source, begin);
    const View trimmed = Trim(first.text);
    if (!Starts(trimmed, L"$$")) return false;
    block.kind = Kind::Math;
    auto pushMathSource = [&](View rawLine) {
        const View t = Trim(rawLine);
        if (t.empty()) return;
        Text formatted;
        AppendMathTex(formatted, t, MathFlag, {});
        for (auto &seg : SplitTextLines(formatted)) {
            if (!Trim(seg.value).empty()) block.lines.push_back(std::move(seg));
        }
    };
    const View afterOpen = Trim(trimmed.substr(2));
    if (afterOpen.size() >= 2 && afterOpen.substr(afterOpen.size() - 2) == L"$$") {
        pushMathSource(afterOpen.substr(0, afterOpen.size() - 2));
        offset = first.next;
        return true;
    }
    pushMathSource(afterOpen);
    size_t pos = first.next;
    while (pos < source.size()) {
        const auto cur = Line(source, pos);
        pos = cur.next;
        const View t = Trim(cur.text);
        if (Starts(t, L"$$")) break;
        if (t.size() >= 2 && t.substr(t.size() - 2) == L"$$") {
            pushMathSource(t.substr(0, t.size() - 2));
            break;
        }
        pushMathSource(cur.text);
    }
    offset = pos;
    return true;
}

bool HasTrailingHardBreak(View rawLine) {
    if (rawLine.size() >= 2 && rawLine[rawLine.size() - 1] == L' ' && rawLine[rawLine.size() - 2] == L' ')
        return true;
    const View trimmed = Trim(rawLine);
    if (!trimmed.empty() && trimmed.back() == L'\\' && !Escaped(trimmed, trimmed.size() - 1)) return true;
    const size_t lt = trimmed.rfind(L'<');
    return lt != View::npos && MatchBrTag(trimmed.substr(lt)) == trimmed.size() - lt;
}

View StripHardBreakSuffix(View rawLine) {
    if (rawLine.size() >= 2 && rawLine[rawLine.size() - 1] == L' ' && rawLine[rawLine.size() - 2] == L' ') {
        while (!rawLine.empty() && rawLine.back() == L' ') rawLine.remove_suffix(1);
        return rawLine;
    }
    const View trimmed = Trim(rawLine);
    if (!trimmed.empty() && trimmed.back() == L'\\' && !Escaped(trimmed, trimmed.size() - 1))
        return trimmed.substr(0, trimmed.size() - 1);
    return trimmed;
}

bool NeedsJoinSpace(wchar_t left, wchar_t right) {
    return left < 0x80 && right < 0x80 && !Space(left) && !Space(right);
}

bool StartsBlockConstruct(View source, size_t offset, View lineText) {
    const View trimmed = Trim(lineText);
    if (trimmed.empty()) return true;
    if (FenceAt(lineText) || HorizontalRule(trimmed) || MatchHeadingLine(lineText) ||
        StripQuoteMarkers(lineText).first > 0 || MatchListLine(lineText) || Starts(trimmed, L"$$") ||
        StartsNoCase(trimmed, L"<details") || StartsNoCase(trimmed, L"<table"))
        return true;
    const auto headings = Cells(lineText);
    if (!headings.empty() && offset < source.size()) {
        const auto next = Line(source, offset);
        const auto align = Divider(next.text);
        if (!align.empty() && headings.size() == align.size()) return true;
    }
    return false;
}

std::vector<Block> ParseInternal(View source, bool proseMarkup, int depth, bool inlineFormatted) {
    std::vector<Block> blocks;
    for (size_t offset = 0; offset < source.size();) {
        const auto line = Line(source, offset);
        offset = line.next;
        if (proseMarkup && Trim(line.text).empty()) continue;

        Block block;
        block.source = line.begin;
        if (const auto fence = FenceAt(line.text)) {
            block.kind = Kind::Code;
            const auto endTag = fence->tag.find_first_of(L" \t");
            block.language = std::wstring(fence->tag.substr(0, std::min<size_t>(endTag, 48)));
            block.codeBegin = block.codeEnd = offset;
            Highlighter highlight(LanguageFor(block.language));
            while (offset < source.size()) {
                const auto body = Line(source, offset);
                offset = body.next;
                const auto close = FenceAt(body.text);
                if (close && close->marker == fence->marker && close->length >= fence->length && close->tag.empty()) break;
                block.lines.push_back(highlight.Highlight(body.text));
                block.codeEnd = offset;
            }
            blocks.push_back(std::move(block));
            continue;
        }

        if (!proseMarkup) {
            if (inlineFormatted && !Trim(line.text).empty()) {
                for (auto &seg : SplitTextLines(Inline(line.text)))
                    block.lines.push_back(std::move(seg));
            } else {
                block.lines.push_back(Text{std::wstring(line.text), {}, {}});
            }
            blocks.push_back(std::move(block));
            continue;
        }

        if (StartsNoCase(Trim(line.text), L"<details") && TryParseDetailsBlock(source, offset, line.begin, block, depth)) {
            blocks.push_back(std::move(block));
            continue;
        }

        if (StartsNoCase(Trim(line.text), L"<table") && TryParseHtmlTable(source, offset, line.begin, block)) {
            blocks.push_back(std::move(block));
            continue;
        }

        if (Starts(Trim(line.text), L"$$") && TryParseMathBlock(source, offset, line.begin, block)) {
            blocks.push_back(std::move(block));
            continue;
        }

        if (line.ended && offset < source.size()) {
            const auto next = Line(source, offset);
            const auto headings = Cells(line.text);
            auto alignment = headings.empty() ? std::vector<Alignment>{} : Divider(next.text);
            if (!alignment.empty() && headings.size() == alignment.size() && alignment.size() <= MaxTableCells) {
                block.kind = Kind::Table;
                block.alignment = std::move(alignment);
                auto &header = block.rows.emplace_back();
                for (auto cell : headings) header.push_back(Inline(cell));
                offset = next.next;
                while (offset < source.size() && block.rows.size() < MaxTableCells / block.alignment.size()) {
                    const auto row = Line(source, offset);
                    if (FenceAt(row.text)) break;
                    const auto cells = Cells(row.text);
                    if (cells.empty()) break;
                    auto &values = block.rows.emplace_back();
                    for (size_t column = 0; column < block.alignment.size(); ++column) {
                        auto cell = column < cells.size() ? cells[column] : View{};
                        if (column + 1 == block.alignment.size() && cells.size() > block.alignment.size())
                            cell = View(cell.data(), static_cast<size_t>(cells.back().data() + cells.back().size() - cell.data()));
                        values.push_back(Inline(cell));
                    }
                    offset = row.next;
                }
                blocks.push_back(std::move(block));
                continue;
            }
        }

        if (const auto q0 = StripQuoteMarkers(line.text); q0.first > 0) {
            block.kind = Kind::Quote;
            std::vector<std::pair<int, View>> rawQuoteLines;
            rawQuoteLines.push_back(q0);
            while (offset < source.size()) {
                const auto nextLine = Line(source, offset);
                const auto nextQ = StripQuoteMarkers(nextLine.text);
                if (nextQ.first <= 0) break;
                offset = nextLine.next;
                rawQuoteLines.push_back(nextQ);
            }
            if (!rawQuoteLines.empty() && rawQuoteLines.front().first == 1) {
                View firstBody = rawQuoteLines.front().second;
                const AlertKind detected = MatchAlertTag(firstBody);
                if (detected != AlertKind::None) {
                    block.alert = detected;
                    if (Trim(firstBody).empty()) {
                        rawQuoteLines.erase(rawQuoteLines.begin());
                    } else {
                        rawQuoteLines.front().second = firstBody;
                    }
                }
            }
            for (const auto &[qDepth, qText] : rawQuoteLines) {
                if (const auto h = MatchHeadingLine(qText)) {
                    for (auto &seg : SplitTextLines(Inline(h->second))) {
                        block.lines.push_back(std::move(seg));
                        block.lineMeta.push_back({0, qDepth, h->first, 0, false, false, false, false});
                    }
                } else if (const auto l = MatchListLine(qText)) {
                    Text prefixText;
                    size_t markerChars = 0;
                    if (!l->isTask) {
                        const std::wstring marker = l->ordered ? std::wstring(l->prefix) : L"• ";
                        markerChars = marker.size();
                        prefixText.value = marker;
                    }
                    AppendInline(prefixText, l->content);
                    bool firstSeg = true;
                    for (auto &seg : SplitTextLines(prefixText)) {
                        block.lines.push_back(std::move(seg));
                        LineMeta m{};
                        m.quoteDepth = qDepth;
                        m.indent = std::min(3, static_cast<int>(l->indentCols / 2));
                        m.markerChars = markerChars;
                        m.isList = true;
                        m.isTask = l->isTask && firstSeg;
                        m.checked = l->checked;
                        m.continuation = !firstSeg;
                        block.lineMeta.push_back(m);
                        firstSeg = false;
                    }
                } else {
                    const auto split = SplitTextLines(Inline(qText));
                    for (size_t s = 0; s < split.size(); ++s) {
                        block.lines.push_back(split[s]);
                        block.lineMeta.push_back({0, qDepth, 0, 0, s > 0, false, false, false});
                    }
                }
            }
            blocks.push_back(std::move(block));
            continue;
        }

        if (const auto h = MatchHeadingLine(line.text)) {
            block.kind = Kind::Heading;
            block.headingLevel = h->first;
            for (auto &seg : SplitTextLines(Inline(h->second))) block.lines.push_back(std::move(seg));
            if (block.lines.empty()) block.lines.emplace_back();
            blocks.push_back(std::move(block));
            continue;
        }

        if (HorizontalRule(Trim(line.text))) {
            block.kind = Kind::Rule;
            blocks.push_back(std::move(block));
            continue;
        }

        if (MatchListLine(line.text)) {
            block.kind = Kind::List;
            std::vector<size_t> indentStack;
            auto levelForCols = [&](size_t cols) {
                while (!indentStack.empty() && cols < indentStack.back()) indentStack.pop_back();
                if (indentStack.empty()) {
                    if (cols > 0) indentStack.push_back(cols);
                    return static_cast<int>(indentStack.size());
                }
                if (cols > indentStack.back()) indentStack.push_back(cols);
                return std::min(6, static_cast<int>(indentStack.size()));
            };
            auto appendListSegment = [&](View segLine) {
                const auto lm = MatchListLine(segLine);
                if (!lm) return false;
                const int level = levelForCols(lm->indentCols);
                Text itemText;
                size_t markerChars = 0;
                if (!lm->isTask) {
                    const std::wstring marker = lm->ordered ? std::wstring(lm->prefix) : L"• ";
                    markerChars = marker.size();
                    itemText.value = marker;
                }
                AppendInline(itemText, lm->content);
                const auto split = SplitTextLines(itemText);
                for (size_t s = 0; s < split.size(); ++s) {
                    block.lines.push_back(split[s]);
                    LineMeta meta{};
                    meta.indent = level;
                    meta.markerChars = markerChars;
                    meta.isList = true;
                    meta.isTask = lm->isTask && s == 0;
                    meta.checked = lm->checked;
                    meta.continuation = s > 0;
                    block.lineMeta.push_back(meta);
                }
                return true;
            };
            auto consumePhysicalListLine = [&](View physical) {
                const auto parts = SplitTopLevelBr(physical);
                if (parts.empty()) return;
                appendListSegment(parts[0]);
                for (size_t p = 1; p < parts.size(); ++p) {
                    const View part = Trim(parts[p]);
                    if (part.empty()) continue;
                    if (appendListSegment(part)) continue;
                    const LineMeta &prev = block.lineMeta.back();
                    for (auto &seg : SplitTextLines(Inline(part))) {
                        block.lines.push_back(std::move(seg));
                        LineMeta cont = prev;
                        cont.continuation = true;
                        cont.isTask = false;
                        block.lineMeta.push_back(cont);
                    }
                }
            };
            consumePhysicalListLine(line.text);
            while (offset < source.size()) {
                const auto nextLine = Line(source, offset);
                if (!MatchListLine(nextLine.text)) break;
                offset = nextLine.next;
                consumePhysicalListLine(nextLine.text);
            }
            blocks.push_back(std::move(block));
            continue;
        }

        // Prose block: join soft-wrapped lines within a paragraph; break on hard break (<br>, 2 spaces, \).
        std::wstring currentParagraph;
        auto flushCurrent = [&]() {
            if (currentParagraph.empty()) return;
            for (auto &seg : SplitTextLines(Inline(currentParagraph))) block.lines.push_back(std::move(seg));
            currentParagraph.clear();
        };
        auto consumeProseLine = [&](View raw, bool *hardBreak) {
            *hardBreak = HasTrailingHardBreak(raw);
            const View piece = Trim(StripHardBreakSuffix(raw));
            if (currentParagraph.empty()) {
                currentParagraph.assign(piece);
            } else if (!piece.empty()) {
                if (NeedsJoinSpace(currentParagraph.back(), piece.front())) currentParagraph.push_back(L' ');
                currentParagraph.append(piece);
            }
            if (*hardBreak) flushCurrent();
        };
        bool hardBreak = false;
        consumeProseLine(line.text, &hardBreak);
        while (offset < source.size()) {
            const auto next = Line(source, offset);
            if (StartsBlockConstruct(source, next.next, next.text)) break;
            offset = next.next;
            consumeProseLine(next.text, &hardBreak);
        }
        flushCurrent();
        if (block.lines.empty()) block.lines.emplace_back();
        blocks.push_back(std::move(block));
    }
    return blocks;
}

} // namespace

std::vector<Text> SplitTextLines(const Text &input) {
    if (input.value.find(L'\n') == std::wstring::npos) return {input};
    std::vector<Text> result;
    const size_t n = input.value.size();
    for (size_t segStart = 0; segStart <= n;) {
        const size_t lf = input.value.find(L'\n', segStart);
        const size_t segEnd = lf == std::wstring::npos ? n : lf;
        if (segStart == n && !result.empty() && n > 0 && input.value.back() == L'\n') break;
        Text part;
        part.value = input.value.substr(segStart, segEnd - segStart);
        for (const auto &span : input.spans) {
            if (span.start + span.length <= segStart) continue;
            if (span.start >= segEnd) break;
            const size_t s = std::max(span.start, segStart);
            const size_t e = std::min(span.start + span.length, segEnd);
            if (e > s) part.spans.push_back({s - segStart, e - s, span.ink, span.flags});
        }
        for (const auto &link : input.links) {
            if (link.start + link.length <= segStart) continue;
            if (link.start >= segEnd) break;
            const size_t s = std::max(link.start, segStart);
            const size_t e = std::min(link.start + link.length, segEnd);
            if (e > s) part.links.push_back({s - segStart, e - s, link.url});
        }
        result.push_back(std::move(part));
        if (lf == std::wstring::npos) break;
        segStart = lf + 1;
    }
    return result;
}

std::vector<Block> Parse(View source, bool proseMarkup, bool inlineFormatted) {
    return ParseInternal(source, proseMarkup, 0, inlineFormatted);
}

} // namespace serialctl::ai::markup
