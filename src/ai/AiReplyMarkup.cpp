#include "AiReplyMarkup.h"
#include <algorithm>
#include <cwctype>
#include <optional>

namespace serialctl::ai::markup {
namespace {
using View = std::wstring_view;
// A short row with few cells can otherwise expand to millions of empty
// native cells when the reply has an exceptionally wide header.
constexpr size_t MaxTableCells = 32768;
bool Starts(View text, View prefix) { return text.substr(0, prefix.size()) == prefix; }
bool Space(wchar_t c) { return c == L' ' || c == L'\t'; }
View Trim(View text) {
    while (!text.empty() && Space(text.front())) text.remove_prefix(1);
    while (!text.empty() && Space(text.back())) text.remove_suffix(1);
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

void AppendInline(Text &result, View text, bool bold = false) {
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'\\' && i + 1 < text.size() && View(L"\\|`*_").find(text[i + 1]) != View::npos) {
            result.value += text[i + 1];
            i += 2;
        } else if (text[i] == L'`') {
            const auto ticks = Run(text, i, L'`');
            const auto end = ClosingTicks(text, i + ticks, ticks);
            if (end == View::npos) {
                result.value.append(text.substr(i, ticks));
                i += ticks;
                continue;
            }
            auto content = text.substr(i + ticks, end - i - ticks);
            if (content.size() > 2 && content.front() == L' ' && content.back() == L' ' && !Trim(content).empty()) {
                content.remove_prefix(1);
                content.remove_suffix(1);
            }
            result.spans.push_back({result.value.size(), content.size(), Ink::InlineCode});
            result.value.append(content);
            i = end + ticks;
        } else if (!bold && (Starts(text.substr(i), L"**") || Starts(text.substr(i), L"__"))) {
            const auto mark = text.substr(i, 2);
            auto end = text.find(mark, i + 2);
            while (end != View::npos && Escaped(text, end)) end = text.find(mark, end + 2);
            if (end == View::npos || end == i + 2) {
                result.value.append(mark);
                i += 2;
                continue;
            }
            const auto start = result.value.size();
            const auto first = result.spans.size();
            AppendInline(result, text.substr(i + 2, end - i - 2), true);
            auto edge = start;
            const auto last = result.spans.size();
            for (size_t index = first; index < last; ++index) {
                const auto code = result.spans[index];
                if (edge < code.start) result.spans.push_back({edge, code.start - edge, Ink::Bold});
                edge = code.start + code.length;
            }
            if (edge < result.value.size()) result.spans.push_back({edge, result.value.size() - edge, Ink::Bold});
            std::sort(result.spans.begin() + first, result.spans.end(), [](const Span &a, const Span &b) {
                return a.start < b.start;
            });
            i = end + 2;
        } else {
            result.value += text[i++];
        }
    }
}
Text Inline(View line) {
    Text result;
    AppendInline(result, line);
    return result;
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

enum class Language { Plain, JavaScript, TypeScript, Python, Java, CSharp, Cpp, Bash, Sql, Html, Css, Json };
Language LanguageFor(View tag) {
    const auto name = Lower(tag);
    if (name == L"js" || name == L"javascript" || name == L"jsx" || name == L"mjs") return Language::JavaScript;
    if (name == L"ts" || name == L"typescript" || name == L"tsx") return Language::TypeScript;
    if (name == L"py" || name == L"python" || name == L"python3") return Language::Python;
    if (name == L"java") return Language::Java;
    if (name == L"cs" || name == L"csharp" || name == L"c#") return Language::CSharp;
    if (name == L"c" || name == L"c++" || name == L"cpp" || name == L"cxx" || name == L"h") return Language::Cpp;
    if (name == L"bash" || name == L"sh" || name == L"shell" || name == L"zsh" || name == L"shellscript") return Language::Bash;
    if (name == L"sql" || name == L"mysql" || name == L"postgresql" || name == L"sqlite" || name == L"tsql") return Language::Sql;
    if (name == L"html" || name == L"xml" || name == L"svg") return Language::Html;
    if (name == L"css" || name == L"scss") return Language::Css;
    if (name == L"json" || name == L"jsonc" || name == L"json5") return Language::Json;
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
    case L::Bash: return KeywordIn(L"case do done elif else esac fi for function if in select then time until while alias cd declare echo eval exec exit export local printf read return set shift source test trap unset wait", word);
    case L::Sql: return KeywordIn(L"add all alter and as asc begin between by case check column commit create cross database default delete desc distinct drop else end exists false foreign from full group having in index inner insert into is join left like limit not null offset on or order outer primary references returning right rollback select set table then true union unique update values view when where with", Lower(word));
    case L::Json: return word == L"true" || word == L"false" || word == L"null";
    default: return false;
    }
}

class Highlighter {
  public:
    explicit Highlighter(Language language) : language_(language) {}
    Text Highlight(View line) {
        Text result{std::wstring(line), {}};
        using L = Language;
        if (language_ == L::Plain) return result;
        const bool cFamily = language_ == L::Cpp || language_ == L::CSharp || language_ == L::Java ||
            language_ == L::JavaScript || language_ == L::TypeScript || language_ == L::Json;
        auto add = [&](size_t start, size_t end, Ink ink) {
            if (end > start) result.spans.push_back({start, end - start, ink});
        };
        for (size_t i = 0; i < line.size();) {
            if (!terminator_.empty()) {
                auto end = line.find(terminator_, i);
                while (end != View::npos && quote_ && Escaped(line, end))
                    end = line.find(terminator_, end + terminator_.size());
                if (end == View::npos) {
                    add(i, line.size(), continuation_);
                    break;
                }
                end += terminator_.size();
                add(i, end, continuation_);
                terminator_ = {};
                i = end;
                continue;
            }
            const auto tail = line.substr(i);
            const auto ch = line[i];
            const bool hashComment = ch == L'#' && (language_ == L::Python ||
                (language_ == L::Bash && (i == 0 || Space(line[i - 1]) || View(L";|&()").find(line[i - 1]) != View::npos)));
            if (hashComment || (cFamily && Starts(tail, L"//")) || (language_ == L::Sql && Starts(tail, L"--"))) {
                add(i, line.size(), Ink::Comment);
                break;
            }
            View close;
            size_t open = 0;
            Ink ink = Ink::Comment;
            bool quote = false;
            if ((cFamily || language_ == L::Css || language_ == L::Sql) && Starts(tail, L"/*")) {
                open = 2; close = L"*/";
            } else if (language_ == L::Html && Starts(tail, L"<!--")) {
                open = 4; close = L"-->";
            } else if (language_ == L::Python && (Starts(tail, L"\"\"\"") || Starts(tail, L"'''"))) {
                open = 3; close = tail.substr(0, 3); ink = Ink::String; quote = true;
            } else if ((language_ == L::JavaScript || language_ == L::TypeScript) && ch == L'`') {
                open = 1; close = tail.substr(0, 1); ink = Ink::String; quote = true;
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
                add(i, end, language_ == L::Json && after != View::npos && line[after] == L':' ? Ink::Name : Ink::String);
                i = end;
                continue;
            }
            if (language_ == L::Cpp && ch == L'#' && Trim(line.substr(0, i)).empty()) {
                add(i, line.size(), Ink::Name);
                break;
            }
            if (language_ == L::Bash && ch == L'$') {
                size_t end = i + 1;
                if (end < line.size() && line[end] == L'{') {
                    const auto closeBrace = line.find(L'}', end + 1);
                    end = closeBrace == View::npos ? end + 1 : closeBrace + 1;
                } else {
                    while (end < line.size() && Word(line[end])) ++end;
                    if (end == i + 1 && end < line.size() && View(L"?#@*!-").find(line[end]) != View::npos) ++end;
                }
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
            if (Word(ch) || (language_ == L::Css && (ch == L'-' || ch == L'@' || ch == L'#'))) {
                size_t end = i + 1;
                while (end < line.size() && (Word(line[end]) || ((language_ == L::Css || language_ == L::Html) && line[end] == L'-'))) ++end;
                const auto after = line.find_first_not_of(L" \t", end);
                if (Keyword(language_, line.substr(i, end - i))) add(i, end, Ink::Keyword);
                else if (language_ == L::Html || (language_ == L::Css && (ch == L'@' || ch == L'#' ||
                    (after != View::npos && line[after] == L':')))) add(i, end, Ink::Name);
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

} // namespace

std::vector<Block> Parse(View source, bool proseMarkup) {
    std::vector<Block> blocks;
    for (size_t offset = 0; offset < source.size();) {
        const auto line = Line(source, offset);
        offset = line.next;
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
        } else if (proseMarkup && line.ended && offset < source.size()) {
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
                    // A partial streamed row keeps its existing cells and the
                    // remaining columns are filled as the next delta arrives.
                    auto &values = block.rows.emplace_back();
                    for (size_t column = 0; column < block.alignment.size(); ++column) {
                        auto cell = column < cells.size() ? cells[column] : View{};
                        if (column + 1 == block.alignment.size() && cells.size() > block.alignment.size())
                            cell = View(cell.data(), static_cast<size_t>(cells.back().data() + cells.back().size() - cell.data()));
                        values.push_back(Inline(cell));
                    }
                    offset = row.next;
                }
            }
        }
        if (block.kind == Kind::Prose) {
            auto text = line.text;
            if (proseMarkup) {
                auto trimmed = Trim(text);
                const auto hashes = Run(trimmed, 0, L'#');
                if (hashes && hashes <= 6 && hashes < trimmed.size() && Space(trimmed[hashes])) {
                    block.kind = Kind::Heading;
                    text = Trim(trimmed.substr(hashes));
                } else if (HorizontalRule(trimmed)) {
                    block.kind = Kind::Rule;
                } else if (trimmed.size() >= 2 && View(L"-*+").find(trimmed.front()) != View::npos && Space(trimmed[1])) {
                    block.kind = Kind::List;
                    auto content = Inline(Trim(trimmed.substr(2)));
                    for (auto &span : content.spans) span.start += 2;
                    content.value.insert(0, L"• ");
                    block.lines.push_back(std::move(content));
                } else {
                    const auto digits = trimmed.find_first_not_of(L"0123456789");
                    if (digits != View::npos && digits > 0 && digits + 1 < trimmed.size() &&
                        (trimmed[digits] == L'.' || trimmed[digits] == L')') && Space(trimmed[digits + 1])) {
                        block.kind = Kind::List;
                        text = trimmed;
                    }
                }
            }
            if (block.kind != Kind::Rule && block.lines.empty())
                block.lines.push_back(proseMarkup ? Inline(text) : Text{std::wstring(text), {}});
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}

} // namespace serialctl::ai::markup
