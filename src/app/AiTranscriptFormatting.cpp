#include "AiPane.h"
#include "Win32Helpers.h"
#include <cwctype>
#include <numeric>

namespace serialctl {
namespace {
using ai::markup::AlertKind;
using ai::markup::Alignment;
using ai::markup::Block;
using ai::markup::Ink;
using ai::markup::Kind;
using ai::markup::LineMeta;
using ai::markup::Span;
using ai::markup::Text;

RECT Moved(RECT rect, int x, int y) {
    OffsetRect(&rect, x, y);
    return rect;
}

void Fill(HDC dc, const RECT &rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Label(HDC dc, const std::wstring &text, RECT rect, HFONT font, COLORREF color) {
    const auto old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
              DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

COLORREF SyntaxInk(Ink ink, unsigned flags, bool dark, COLORREF plain, COLORREF muted, bool user, bool pMuted) {
    if (user) return plain;
    switch (ink) {
    case Ink::Keyword: return dark ? RGB(202, 168, 251) : RGB(106, 53, 157);
    case Ink::String:  return dark ? RGB(164, 216, 157) : RGB(27, 103, 50);
    case Ink::Number:  return dark ? RGB(255, 185, 142) : RGB(146, 61, 24);
    case Ink::Comment: return dark ? RGB(166, 173, 180) : RGB(96, 98, 106);
    case Ink::Name:    return dark ? RGB(142, 204, 244) : RGB(21, 84, 140);
    case Ink::Link:    return UiAccentInk(dark);
    case Ink::Muted:   return muted;
    default: break;
    }
    if (flags & ai::markup::LinkFlag) return UiAccentInk(dark);
    if ((flags & ai::markup::MutedFlag) || pMuted) return muted;
    return plain;
}

std::pair<const wchar_t *, COLORREF> AlertStyle(AlertKind alert, bool dark, COLORREF plain) {
    switch (alert) {
    case AlertKind::Note:      return {L"说明 · NOTE", UiAccentInk(dark)};
    case AlertKind::Tip:       return {L"建议 · TIP", dark ? RGB(164, 216, 157) : RGB(27, 103, 50)};
    case AlertKind::Important: return {L"重要 · IMPORTANT", dark ? RGB(202, 168, 251) : RGB(106, 53, 157)};
    case AlertKind::Warning:   return {L"警告 · WARNING", dark ? RGB(255, 185, 142) : RGB(146, 61, 24)};
    case AlertKind::Caution:   return {L"危险 · CAUTION", UiAiTheme(dark).danger};
    default: return {L"", plain};
    }
}

int HeadingStyleForLevel(int level) {
    if (level <= 1) return 1;
    if (level == 2) return 4;
    if (level == 3) return 5;
    if (level == 4) return 6;
    return 7;
}

bool LowSurrogate(wchar_t c) { return c >= 0xdc00 && c <= 0xdfff; }

bool IsCjkWrapChar(wchar_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x3040 && c <= 0x30FF);
}

bool IsLineStartForbidden(wchar_t c) {
    constexpr std::wstring_view forbidden =
        L".,;:!?)]}%'\"\uFF0C\u3002\uFF1B\uFF1A\u3001\uFF01\uFF1F\uFF09\u3011\u300B\u300D\u300F\u201D\u2019\u2026\u2014\u00B7\uFF5E";
    return forbidden.find(c) != std::wstring_view::npos;
}

bool IsLineEndForbidden(wchar_t c) {
    constexpr std::wstring_view forbidden = L"([{'\"\uFF08\u3010\u300A\u300C\u300E\u201C\u2018";
    return forbidden.find(c) != std::wstring_view::npos;
}

bool IsCjkTrailingBreakPunct(wchar_t c) {
    constexpr std::wstring_view trailing =
        L"\uFF0C\u3002\uFF1B\uFF1A\u3001\uFF01\uFF1F\uFF09\u3011\u300B\u300D\u300F\u201D\u2019\u2026\uFF5E";
    return trailing.find(c) != std::wstring_view::npos;
}

bool CanBreakBefore(const std::wstring &text, size_t idx) {
    if (idx == 0 || idx >= text.size()) return false;
    const wchar_t left = text[idx - 1];
    const wchar_t right = text[idx];
    if (left == L' ' || left == L'\t') return !IsLineStartForbidden(right);
    if (IsLineEndForbidden(left) || IsLineStartForbidden(right)) return false;
    return IsCjkWrapChar(left) || IsCjkTrailingBreakPunct(left) || IsCjkWrapChar(right);
}

bool EqualsNoCaseAscii(const std::wstring &value, const wchar_t *target) {
    size_t i = 0;
    for (; i < value.size() && target[i]; ++i) {
        if (std::towlower(value[i]) != std::towlower(target[i])) return false;
    }
    return i == value.size() && target[i] == L'\0';
}

bool IsAnchorDroppedPunct(wchar_t c) {
    if (c == L'-' || c == L'_') return false;
    if (c < 0x80) return !std::iswalnum(c);
    constexpr std::wstring_view fullwidth = L"（）：、，。《》“”‘’！？【】；—…·";
    return fullwidth.find(c) != std::wstring_view::npos;
}

std::wstring AnchorSlug(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (wchar_t ch : raw) {
        if (ch == L' ' || ch == L'\t') {
            out.push_back(L'-');
        } else if (!IsAnchorDroppedPunct(ch)) {
            out.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }
    }
    return out;
}

// Keep one measurement and one source index per UTF-16 code unit. The same
// advances drive wrapping, clipping, selection and horizontal hit testing.
void MeasureRun(HDC dc, const std::wstring &text, size_t start, size_t end, std::vector<int> &advances) {
    if (start >= end) return;
    SIZE blank{};
    GetTextExtentPoint32W(dc, L" ", 1, &blank);
    const int tab = std::max(1L, blank.cx * 4);
    int widths[512];
    while (start < end) {
        if (text[start] == L'\n') {
            advances[start + 1] = advances[start];
            ++start;
            continue;
        }
        if (text[start] == L'\t') {
            advances[start + 1] = (advances[start] / tab + 1) * tab;
            ++start;
            continue;
        }
        size_t count = 1;
        while (count < std::size(widths) && start + count < end &&
               text[start + count] != L'\t' && text[start + count] != L'\n')
            ++count;
        if (start + count < end && LowSurrogate(text[start + count])) --count;
        if (!count) count = std::min<size_t>(2, end - start);
        SIZE size{};
        if (!GetTextExtentExPointW(dc, text.data() + start, static_cast<int>(count), INT_MAX,
                                   nullptr, widths, &size))
            std::fill_n(widths, count, 0);
        const auto base = advances[start];
        for (size_t i = 0; i < count; ++i) advances[start + i + 1] = base + widths[i];
        start += count;
    }
}

void DrawBoxClipped(HDC dc, RECT rect, const RECT &clip, COLORREF fill, COLORREF border, int radius) {
    RECT r{};
    if (!IntersectRect(&r, &rect, &clip)) return;
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
    UiBox(dc, rect, fill, border, radius);
    RestoreDC(dc, saved);
}

} // namespace

HFONT AiPane::ParagraphFont(int style, Ink ink, unsigned flags) const {
    if (style == 2) return codeFont_;
    if ((flags & ai::markup::InlineCodeFlag) || ink == Ink::InlineCode ||
        (flags & ai::markup::KbdFlag) || ink == Ink::Kbd)
        return codeFont_;
    if ((flags & (ai::markup::SubFlag | ai::markup::SupFlag)) || ink == Ink::Sub || ink == Ink::Sup)
        return subFont_ ? subFont_ : smallFont_;
    if (style == 1) return h1Font_ ? h1Font_ : titleFont_;
    if (style == 4) return titleFont_;
    if (style == 5) return h3Font_ ? h3Font_ : titleFont_;
    if (style == 6 || style == 7) return boldFont_ ? boldFont_ : font_;
    const bool isBold = style == 3 || (flags & ai::markup::BoldFlag) != 0 ||
                        ink == Ink::Bold || ink == Ink::BoldItalic;
    const bool isItalic = (flags & ai::markup::ItalicFlag) != 0 ||
                          ink == Ink::Italic || ink == Ink::BoldItalic;
    if (isBold && isItalic) return boldItalicFont_ ? boldItalicFont_ : (boldFont_ ? boldFont_ : font_);
    if (isBold) return boldFont_ ? boldFont_ : font_;
    if (isItalic) return italicFont_ ? italicFont_ : font_;
    return font_;
}

void AiPane::LayoutMessage(HDC dc, Message &message, int width) {
    const auto previous = std::move(message.blocks);
    message.paragraphs.clear();
    message.blocks.clear();
    message.taskChecks.clear();
    message.headingRules.clear();
    const bool user = message.role == L"我";
    const bool isThinking = message.role == L"思考";
    const bool isTool = message.role == L"工具";
    std::wstring sourceText = message.text;
    if (isThinking && !sourceText.empty()) {
        // Normalize streamed thinking summaries like "**Step title**Details..." into clean paragraph breaks.
        std::wstring normalized;
        normalized.reserve(sourceText.size() + 32);
        bool inFence = false;
        size_t pos = 0;
        while (pos < sourceText.size()) {
            size_t lineEnd = sourceText.find(L'\n', pos);
            const bool hasNewline = lineEnd != std::wstring::npos;
            if (!hasNewline) lineEnd = sourceText.size();
            const std::wstring_view line(sourceText.data() + pos, lineEnd - pos);
            const auto trimmedStart = line.find_first_not_of(L" \t\r");
            if (trimmedStart != std::wstring_view::npos && line.substr(trimmedStart, 3) == L"```")
                inFence = !inFence;
            if (!inFence && trimmedStart != std::wstring_view::npos && line.substr(trimmedStart, 2) == L"**") {
                const auto close = line.find(L"**", trimmedStart + 2);
                if (close != std::wstring_view::npos && close > trimmedStart + 2 && close - (trimmedStart + 2) <= 96) {
                    const size_t after = close + 2;
                    if (after < line.size() && line[after] != L' ' && line[after] != L'\t' &&
                        line[after] != L'\r' && line[after] != L'*') {
                        if (!normalized.empty() && normalized.back() != L'\n')
                            normalized += L"\n\n";
                        normalized.append(line.substr(0, after));
                        normalized += L"\n\n";
                        normalized.append(line.substr(after));
                        if (hasNewline) normalized.push_back(L'\n');
                        pos = hasNewline ? lineEnd + 1 : lineEnd;
                        continue;
                    }
                }
            }
            normalized.append(line);
            if (hasNewline) normalized.push_back(L'\n');
            pos = hasNewline ? lineEnd + 1 : lineEnd;
        }
        sourceText = std::move(normalized);
    }
    auto parsed = ai::markup::Parse(sourceText, user || message.role == L"AI" || isThinking, isTool);
    const auto originalFont = GetCurrentObject(dc, OBJ_FONT);
    int y = D(12) + (message.groupHeader ? D(28) : 0) + (isThinking ? D(20) : 0);
    const int margin = D(16), right = width - margin;

    const auto measure = [&](const std::wstring &text, const std::vector<Span> &spans, int style, size_t limit) {
        const auto isPillSpan = [](const Span &sp) {
            return (sp.flags & (ai::markup::InlineCodeFlag | ai::markup::KbdFlag)) != 0 ||
                   sp.ink == Ink::InlineCode || sp.ink == Ink::Kbd;
        };
        const int pillPad = D(4);
        std::vector<int> advances(limit + 1);
        size_t end = 0;
        if (style != 2) {
            for (const auto &span : spans) {
                if (span.start >= limit) break;
                const size_t s = std::max(end, span.start);
                SelectObject(dc, ParagraphFont(style));
                MeasureRun(dc, text, end, s, advances);
                SelectObject(dc, ParagraphFont(style, span.ink, span.flags));
                end = std::max(s, std::min(limit, span.start + span.length));
                const bool pill = end > s && isPillSpan(span);
                if (pill && s > 0) {
                    const bool spaceBefore = text[s - 1] == L' ' || text[s - 1] == L'\t';
                    advances[s] += spaceBefore ? pillPad : pillPad + D(2);
                }
                MeasureRun(dc, text, s, end, advances);
                if (pill) advances[end] += pillPad + D(2);
            }
        }
        SelectObject(dc, ParagraphFont(style));
        MeasureRun(dc, text, end, limit, advances);
        return advances;
    };

    auto paragraph = [&](Text text, int style, int left, int edge, int top, bool wrap,
                         size_t block = SIZE_MAX, Alignment alignment = Alignment::Left, bool tabAfter = false,
                         int hangingIndent = 0, bool muted = false, int headingLevel = 0) {
        Paragraph item{{left, top, edge, top}, std::move(text.value), style, {},
                       std::move(text.spans), std::move(text.links), block, tabAfter, muted, headingLevel};
        auto advances = measure(item.text, item.spans, style, item.text.size());
        SelectObject(dc, ParagraphFont(style));
        TEXTMETRICW metric{};
        GetTextMetricsW(dc, &metric);
        const int lineHeight = style == 0 && item.text.empty() && block == SIZE_MAX
            ? D(8) : std::max(D(20), static_cast<int>(metric.tmHeight) + D(4));
        const int maxHanging = std::max(0, (edge - left) / 2);
        const int clampedHanging = std::clamp(hangingIndent, 0, maxHanging);
        size_t offset = 0;
        size_t lineIndex = 0;
        do {
            const int lineLeft = lineIndex == 0 ? left : left + clampedHanging;
            const int available = std::max(1, edge - lineLeft);
            const size_t lf = item.text.find(L'\n', offset);
            const size_t segmentEnd = lf == std::wstring::npos ? item.text.size() : lf;
            auto next = segmentEnd;
            bool endedAtNewline = lf != std::wstring::npos;
            if (wrap && segmentEnd > offset && advances[segmentEnd] - advances[offset] > available) {
                const auto fit = std::upper_bound(advances.begin() + offset, advances.begin() + segmentEnd + 1,
                                                  advances[offset] + available);
                next = std::max(offset + 1, static_cast<size_t>(fit - advances.begin() - 1));
                if (next < segmentEnd && LowSurrogate(item.text[next]))
                    next = next == offset + 1 ? next + 1 : next - 1;
                const auto half = offset + (next - offset) / 2;
                size_t brk = next;
                while (brk > half && !CanBreakBefore(item.text, brk)) --brk;
                if (brk > half) next = brk;
                endedAtNewline = false;
            }
            const auto textWidth = advances[next] - advances[offset];
            const auto remaining = std::max(0, available - textWidth);
            const auto shift = alignment == Alignment::Right ? remaining : alignment == Alignment::Center ? remaining / 2 : 0;
            TextLine line{{lineLeft + shift, top, wrap ? edge : lineLeft + std::max(available, textWidth), top + lineHeight},
                          offset, next - offset, {}};
            if (!wrap && lf == std::wstring::npos && lineIndex == 0) {
                line.advances = std::move(advances);
            } else {
                line.advances.reserve(line.length + 1);
                for (auto i = offset; i <= next; ++i) line.advances.push_back(advances[i] - advances[offset]);
            }
            item.lines.push_back(std::move(line));
            top += lineHeight;
            offset = endedAtNewline ? next + 1 : next;
            ++lineIndex;
        } while (offset < item.text.size());
        item.rect.bottom = top;
        message.paragraphs.push_back(std::move(item));
        return top;
    };

    const auto preferredWidth = [&](const Text &text, int style) {
        size_t limit = std::min<size_t>(text.value.size(), 320);
        if (limit < text.value.size() && LowSurrogate(text.value[limit])) --limit;
        const auto advances = measure(text.value, text.spans, style, limit);
        int widest = 0;
        for (size_t segStart = 0; segStart <= limit;) {
            const size_t lf = text.value.find(L'\n', segStart);
            const size_t segEnd = lf == std::wstring::npos || lf > limit ? limit : lf;
            widest = std::max(widest, advances[segEnd] - advances[segStart]);
            if (lf == std::wstring::npos || lf >= limit) break;
            segStart = lf + 1;
        }
        return widest;
    };

    const auto markerPrefixPixels = [&](const Text &text, size_t markerChars, int style) {
        if (!markerChars || markerChars > text.value.size()) return 0;
        return measure(text.value, text.spans, style, markerChars).back();
    };

    auto layoutBlocks = [&](auto &self, const std::vector<Block> &blocks, int leftBound, int rightBound, int &curY,
                            bool insideCard, size_t ownerBlock) -> void {
        const int spanWidth = std::max(1, rightBound - leftBound);
        for (size_t bi = 0; bi < blocks.size(); ++bi) {
            const auto &block = blocks[bi];
            if (block.kind == Kind::Prose) {
                for (auto &line : block.lines)
                    curY = paragraph(line, 0, leftBound, rightBound, curY, true, ownerBlock);
                curY += D(6);
                continue;
            }
            if (block.kind == Kind::Heading) {
                const int level = std::clamp(block.headingLevel ? block.headingLevel : 2, 1, 6);
                const int style = HeadingStyleForLevel(level);
                const bool muted = level >= 6;
                if (bi > 0) curY += level <= 2 ? D(10) : D(6);
                else if (level <= 2) curY += D(2);
                for (size_t li = 0; li < block.lines.size(); ++li)
                    curY = paragraph(block.lines[li], style, leftBound, rightBound, curY, true, ownerBlock,
                                     Alignment::Left, false, 0, muted, li == 0 ? level : 0);
                if (level == 1 && !insideCard) {
                    curY += D(4);
                    message.headingRules.push_back({leftBound, curY, rightBound, curY + 1});
                    curY += D(6);
                } else {
                    curY += level <= 3 ? D(6) : D(4);
                }
                continue;
            }
            if (block.kind == Kind::List) {
                int lastIndentX = 0;
                int lastMarkerWidth = 0;
                for (size_t li = 0; li < block.lines.size(); ++li) {
                    const LineMeta meta = li < block.lineMeta.size() ? block.lineMeta[li] : LineMeta{};
                    if (li > 0 && !meta.continuation) curY += D(3);
                    const int indentX = std::min(std::max(0, spanWidth / 2), meta.indent * D(18));
                    const int itemBase = leftBound + D(6) + indentX;
                    if (!meta.continuation) {
                        lastIndentX = indentX;
                        if (meta.isTask) {
                            const int cbSize = D(14);
                            const int cbTop = curY + D(3);
                            message.taskChecks.push_back({{itemBase, cbTop, itemBase + cbSize, cbTop + cbSize}, meta.checked});
                            lastMarkerWidth = D(22);
                            curY = paragraph(block.lines[li], 0, itemBase + D(22), rightBound, curY, true, ownerBlock);
                        } else {
                            lastMarkerWidth = markerPrefixPixels(block.lines[li], meta.markerChars, 0);
                            curY = paragraph(block.lines[li], 0, itemBase, rightBound, curY, true, ownerBlock,
                                             Alignment::Left, false, lastMarkerWidth);
                        }
                    } else {
                        const int contLeft = leftBound + D(6) + lastIndentX + lastMarkerWidth;
                        curY = paragraph(block.lines[li], 0, contLeft, rightBound, curY, true, ownerBlock);
                    }
                }
                curY += D(6);
                continue;
            }

            if (insideCard) {
                // Inside <details> keep secondary nested blocks lightweight and inline within the card bounds.
                if (block.kind == Kind::Rule) {
                    curY += D(6);
                    message.headingRules.push_back({leftBound, curY, rightBound, curY + 1});
                    curY += D(6);
                } else if (block.kind == Kind::Quote || block.kind == Kind::Math) {
                    for (auto &line : block.lines)
                        curY = paragraph(line, 0, leftBound + D(10), rightBound, curY, true, ownerBlock);
                    curY += D(4);
                } else if (block.kind == Kind::Code) {
                    for (auto &line : block.lines)
                        curY = paragraph(line, 2, leftBound + D(8), rightBound, curY, false, ownerBlock);
                    curY += D(4);
                } else if (block.kind == Kind::Table) {
                    for (const auto &row : block.rows) {
                        std::wstring joined;
                        for (size_t c = 0; c < row.size(); ++c) {
                            if (c) joined += L"  |  ";
                            joined += row[c].value;
                        }
                        curY = paragraph({joined, {}, {}}, 0, leftBound, rightBound, curY, true, ownerBlock);
                    }
                    curY += D(4);
                } else if (block.kind == Kind::Details && !block.children.empty()) {
                    self(self, block.children, leftBound + D(10), rightBound, curY, true, ownerBlock);
                }
                continue;
            }

            const auto index = message.blocks.size();
            ReplyBlock visual;
            visual.kind = block.kind;
            visual.alert = block.alert;
            visual.source = block.source;
            const auto old = std::lower_bound(previous.begin(), previous.end(), block.source,
                [](const ReplyBlock &candidate, size_t source) { return candidate.source < source; });
            const bool existed = old != previous.end() && old->source == block.source && old->kind == block.kind;
            visual.rect = {leftBound, curY, rightBound, curY};
            if (block.kind == Kind::Rule) {
                visual.rect.top += D(8);
                visual.rect.bottom = visual.rect.top + 1;
                curY += D(16);
            } else if (block.kind == Kind::Quote) {
                curY += D(6);
                visual.body = {leftBound + D(12), curY, rightBound - D(8), curY};
                if (block.alert != AlertKind::None) {
                    const auto alertInfo = AlertStyle(block.alert, dark_, 0);
                    curY = paragraph({alertInfo.first, {}, {}}, 3, visual.body.left, visual.body.right, curY, true, index);
                    curY += D(2);
                }
                int lastIndentX = 0, lastMarkerWidth = 0;
                for (size_t li = 0; li < block.lines.size(); ++li) {
                    const LineMeta meta = li < block.lineMeta.size() ? block.lineMeta[li] : LineMeta{};
                    const int depthShift = std::max(0, meta.quoteDepth - 1) * D(12);
                    const int lineStartTop = curY;
                    const int quoteLeft = std::min(visual.body.right - D(40), visual.body.left + depthShift);
                    if (meta.headingLevel > 0) {
                        curY = paragraph(block.lines[li], HeadingStyleForLevel(meta.headingLevel), quoteLeft,
                                         visual.body.right, curY, true, index);
                    } else if (meta.isList) {
                        const int indentX = std::min(std::max(0, static_cast<int>(visual.body.right - quoteLeft) / 2),
                                                     meta.indent * D(16));
                        if (!meta.continuation) {
                            lastIndentX = indentX;
                            if (meta.isTask) {
                                const int cbSize = D(14);
                                const int cbTop = curY + D(3);
                                message.taskChecks.push_back({{quoteLeft + indentX, cbTop,
                                                               quoteLeft + indentX + cbSize, cbTop + cbSize}, meta.checked});
                                lastMarkerWidth = D(22);
                                curY = paragraph(block.lines[li], 0, quoteLeft + indentX + D(22), visual.body.right, curY,
                                                 true, index);
                            } else {
                                lastMarkerWidth = markerPrefixPixels(block.lines[li], meta.markerChars, 0);
                                curY = paragraph(block.lines[li], 0, quoteLeft + indentX, visual.body.right, curY, true,
                                                 index, Alignment::Left, false, lastMarkerWidth);
                            }
                        } else {
                            curY = paragraph(block.lines[li], 0, quoteLeft + lastIndentX + lastMarkerWidth,
                                             visual.body.right, curY, true, index);
                        }
                    } else {
                        curY = paragraph(block.lines[li], 0, quoteLeft, visual.body.right, curY, true, index);
                    }
                    if (meta.quoteDepth >= 2)
                        visual.quoteBands.push_back({meta.quoteDepth, lineStartTop, curY});
                }
                curY += D(6);
                visual.body.bottom = curY;
                curY += D(4);
            } else if (block.kind == Kind::Math) {
                curY += D(10);
                visual.body = {leftBound + D(14), curY, rightBound - D(14), curY};
                for (auto &line : block.lines)
                    curY = paragraph(line, 0, visual.body.left, visual.body.right, curY, true, index, Alignment::Center);
                if (block.lines.empty()) curY += D(20);
                visual.body.bottom = curY;
                curY += D(10);
            } else if (block.kind == Kind::Details) {
                const bool open = block.detailsOpen ^ (message.toggledDetails.count(block.source) != 0);
                visual.expanded = open;
                visual.toggle = {leftBound, curY, rightBound, curY + D(32)};
                visual.body = {leftBound + D(10), curY, rightBound - D(10), curY};
                const int summaryLeft = leftBound + D(28);
                const int headerBottom = paragraph(block.summary, 3, summaryLeft, rightBound - D(10), curY + D(6), true, index);
                visual.toggle.bottom = std::max(curY + D(32), headerBottom + D(6));
                curY = visual.toggle.bottom;
                if (open && !block.children.empty()) {
                    curY += D(6);
                    self(self, block.children, leftBound + D(14), rightBound - D(10), curY, true, index);
                    curY += D(2);
                }
                visual.body.bottom = curY;
                curY += D(4);
            } else if (block.kind == Kind::Code) {
                visual.language = block.language.empty() ? L"代码"
                    : EqualsNoCaseAscii(block.language, L"mermaid") ? L"Mermaid (源码)" : block.language;
                visual.codeBegin = block.codeBegin;
                visual.codeEnd = block.codeEnd;
                visual.copy = {rightBound - D(66), curY + D(2), rightBound - D(4), curY + D(30)};
                curY += D(36);
                visual.body = {leftBound + D(10), curY, rightBound - D(10), curY};
                int widest = 0;
                for (auto &line : block.lines) {
                    curY = paragraph(line, 2, visual.body.left, visual.body.right, curY, false, index);
                    widest = std::max(widest, message.paragraphs.back().lines.front().advances.back());
                }
                if (block.lines.empty()) curY += D(20);
                visual.body.bottom = curY;
                visual.maximum = std::max(0, widest - static_cast<int>(visual.body.right - visual.body.left));
                curY += D(8);
            } else {
                const size_t count = block.alignment.size();
                const int available = std::max(1, rightBound - leftBound);
                const int padding = D(10);
                visual.tableReady = block.rows.size() >= 3 ||
                    (block.rows.size() == 2 && std::all_of(block.rows[1].begin(), block.rows[1].end(),
                                                           [](const Text &cell) { return !cell.value.empty(); }));
                if (existed && old->tableReady && old->columns.size() == count && message.measuredWidth == width &&
                    (block.rows.size() > old->rows.size() || message.measuredLength == message.text.size())) {
                    visual.columns = old->columns;
                } else {
                    visual.columns.resize(count);
                    std::vector<int> wanted(count);
                    for (size_t column = 0; column < count; ++column) {
                        const auto header = preferredWidth(block.rows.front()[column], 3) + padding * 2 + D(2);
                        visual.columns[column] = std::clamp(header, D(56), D(132));
                        wanted[column] = std::min(D(640), std::max(header, visual.columns[column]));
                        for (size_t row = 1; row < std::min<size_t>(block.rows.size(), 24); ++row) {
                            const auto cellWidth = preferredWidth(block.rows[row][column], 0) + padding * 2 + D(2);
                            wanted[column] = std::max(wanted[column], std::min(D(640), cellWidth));
                        }
                        visual.columns[column] = std::min(visual.columns[column], wanted[column]);
                    }
                    const int base = std::accumulate(visual.columns.begin(), visual.columns.end(), 0);
                    const int desired = std::accumulate(wanted.begin(), wanted.end(), 0);
                    const int extra = std::max(0, std::min(available, desired) - base);
                    if (extra && desired > base)
                        for (size_t column = 0; column < count; ++column)
                            visual.columns[column] += static_cast<int>(static_cast<long long>(wanted[column] - visual.columns[column])
                                                                       * extra / (desired - base));
                }
                const auto total = std::accumulate(visual.columns.begin(), visual.columns.end(), 0);
                visual.rect.right = leftBound + std::min(total, available);
                visual.body = visual.rect;
                for (size_t row = 0; row < block.rows.size(); ++row) {
                    int x = leftBound;
                    int bottom = curY;
                    const auto first = message.paragraphs.size();
                    for (size_t column = 0; column < count; ++column) {
                        const int edge = x + visual.columns[column];
                        bottom = std::max(bottom, paragraph(block.rows[row][column], row == 0 ? 3 : 0,
                            x + padding, edge - padding, curY + D(6), true, index, block.alignment[column],
                            column + 1 < count));
                        x = edge;
                    }
                    curY = bottom + D(6);
                    for (size_t p = first; p < message.paragraphs.size(); ++p) message.paragraphs[p].rect.bottom = curY;
                    visual.rows.push_back(curY);
                }
                visual.body.bottom = curY;
                visual.maximum = std::max(0, total - available);
            }
            if (visual.maximum) {
                visual.track = {visual.rect.left + D(6), curY, visual.rect.right - D(6), curY + D(14)};
                if (existed) visual.offset = std::min(visual.maximum, old->offset);
                curY += D(14);
            }
            if (block.kind != Kind::Rule) visual.rect.bottom = curY;
            message.blocks.push_back(std::move(visual));
            curY += D(6);
        }
    };

    layoutBlocks(layoutBlocks, parsed, margin, right, y, false, SIZE_MAX);
    if (originalFont) SelectObject(dc, originalFont);
    message.measuredWidth = width;
    message.measuredLength = message.text.size();
    message.measuredGroupHeader = message.groupHeader;
    message.measuredHeight = y + D(28);
}

RECT AiPane::ReplyScrollThumb(const ReplyBlock &block) const {
    const int track = std::max(1L, block.track.right - block.track.left);
    const int body = std::max(1L, block.body.right - block.body.left);
    const int size = std::min(track, std::max(D(28), static_cast<int>(static_cast<long long>(track) * body /
                                    (static_cast<long long>(body) + block.maximum))));
    const int start = block.track.left + MulDiv(block.offset, track - size, std::max(1, block.maximum));
    return {start, block.track.top, start + size, block.track.bottom};
}

void AiPane::PaintMessageText(HDC dc, const Message &message, size_t index, const RECT &box, const RECT &viewport) {
    const auto theme = UiAiTheme(dark_);
    const bool user = message.role == L"我";
    const COLORREF plain = user ? RGB(255, 255, 255) : theme.text;
    const COLORREF mutedInk = user ? RGB(230, 238, 255) : theme.muted;
    const COLORREF border = user ? UiAccentInset() : theme.border;
    const auto originalFont = GetCurrentObject(dc, OBJ_FONT);
    SetBkMode(dc, TRANSPARENT);
    for (const auto &block : message.blocks) {
        auto rect = Moved(block.rect, box.left, box.top);
        RECT visible{};
        if (!IntersectRect(&visible, &rect, &viewport)) continue;
        if (block.kind == Kind::Rule) {
            UiSeparator(dc, rect.left, rect.right, rect.top, border);
            continue;
        }
        if (block.kind == Kind::Quote || block.kind == Kind::Math) {
            const auto fill = user ? UiAccentInset() : theme.raised;
            RECT clipped = rect;
            clipped.top = std::max(clipped.top, viewport.top - D(6));
            clipped.bottom = std::min(clipped.bottom, viewport.bottom + D(6));
            UiBox(dc, clipped, fill, user ? fill : border, D(6));
            if (block.kind == Kind::Quote) {
                const COLORREF barColor = user ? plain
                    : (block.alert != AlertKind::None ? AlertStyle(block.alert, dark_, plain).second : theme.accent);
                RECT bar = {rect.left, std::max(rect.top + D(2), viewport.top),
                            rect.left + D(3), std::min(rect.bottom - D(2), viewport.bottom)};
                if (bar.bottom > bar.top) Fill(dc, bar, barColor);
                for (const auto &band : block.quoteBands) {
                    for (int d = 2; d <= band.depth; ++d) {
                        const int bx = rect.left + D(12) + (d - 2) * D(12);
                        RECT inner = {bx, std::max(box.top + band.top + D(2), viewport.top),
                                      bx + D(2), std::min(box.top + band.bottom - D(2), viewport.bottom)};
                        if (inner.bottom > inner.top) Fill(dc, inner, user ? plain : border);
                    }
                }
            }
            continue;
        }
        if (block.kind == Kind::Details) {
            const auto fill = user ? UiAccentInset() : theme.raised;
            RECT clipped = rect;
            clipped.top = std::max(clipped.top, viewport.top - D(8));
            clipped.bottom = std::min(clipped.bottom, viewport.bottom + D(8));
            UiBox(dc, clipped, fill, user ? fill : border, D(8));
            const auto toggle = Moved(block.toggle, box.left, box.top);
            if (toggle.bottom > viewport.top && toggle.top < viewport.bottom) {
                UiDrawGlyph(dc, {toggle.left + D(8), toggle.top + D(8), toggle.left + D(24), toggle.top + D(24)},
                            user ? plain : theme.muted,
                            block.expanded ? UiGlyph::ChevronDown : UiGlyph::ChevronRight);
                if (block.expanded)
                    UiSeparator(dc, rect.left + 1, rect.right - 1, toggle.bottom, border);
            }
            continue;
        }
        if (block.kind == Kind::Code) {
            const auto fill = user ? UiAccentInset() : theme.raised;
            rect.top = std::max(rect.top, viewport.top - D(8));
            rect.bottom = std::min(rect.bottom, viewport.bottom + D(8));
            UiBox(dc, rect, fill, user ? fill : border, D(8));
            const auto top = block.rect.top + box.top;
            if (top + D(32) > viewport.top) {
                const auto button = Moved(block.copy, box.left, box.top);
                Label(dc, block.language, {rect.left + D(12), top, button.left - D(8), top + D(32)}, smallFont_,
                      user ? plain : theme.muted);
                UiDrawGlyph(dc, {button.left + D(6), button.top + D(6), button.left + D(22), button.top + D(22)},
                            user ? plain : theme.muted, UiGlyph::Copy);
                Label(dc, L"复制", {button.left + D(27), button.top, button.right, button.bottom}, smallFont_,
                      user ? plain : theme.muted);
                UiSeparator(dc, rect.left + 1, rect.right - 1, top + D(32), border);
            }
        } else {
            const auto body = Moved(block.body, box.left, box.top);
            RECT shown{};
            if (IntersectRect(&shown, &body, &viewport)) {
                Fill(dc, shown, user ? theme.accent : UiTheme(dark_).surface);
                RECT header{body.left, body.top, body.right, block.rows.front() + box.top};
                if (IntersectRect(&header, &header, &shown)) Fill(dc, header, user ? UiAccentInset() : theme.raised);
                const auto first = std::lower_bound(block.rows.begin(), block.rows.end(), shown.top - box.top);
                for (auto row = first; row != block.rows.end() && *row + box.top <= shown.bottom; ++row)
                    UiSeparator(dc, shown.left, shown.right, std::min(shown.bottom - 1, *row + box.top - 1), border);
                if (body.top >= shown.top) UiSeparator(dc, shown.left, shown.right, body.top, border);
                int x = body.left - block.offset;
                for (int column : block.columns) {
                    if (x >= shown.left && x < shown.right) Fill(dc, {x, shown.top, x + 1, shown.bottom}, border);
                    x += column;
                }
                Fill(dc, {shown.right - 1, shown.top, shown.right, shown.bottom}, border);
            }
        }
        if (block.maximum) {
            auto thumb = Moved(ReplyScrollThumb(block), box.left, box.top);
            auto track = Moved(block.track, box.left, box.top);
            Fill(dc, {track.left, track.top + D(6), track.right, track.top + D(8)}, border);
            thumb.top += D(5);
            thumb.bottom = thumb.top + D(4);
            Fill(dc, thumb, user ? plain : theme.muted);
        }
    }

    for (const auto &rule : message.headingRules) {
        const auto moved = Moved(rule, box.left, box.top);
        if (moved.bottom > viewport.top && moved.top < viewport.bottom)
            UiSeparator(dc, std::max(moved.left, viewport.left), std::min(moved.right, viewport.right), moved.top, border);
    }

    for (const auto &check : message.taskChecks) {
        const auto cb = Moved(check.rect, box.left, box.top);
        RECT vis{};
        if (!IntersectRect(&vis, &cb, &viewport)) continue;
        if (user) {
            UiBox(dc, cb, check.checked ? UiAccentInset() : theme.accent, plain, D(2));
            if (check.checked) UiDrawGlyph(dc, cb, plain, UiGlyph::Check);
        } else {
            UiBox(dc, cb, check.checked ? theme.accent : UiTheme(dark_).surface,
                  check.checked ? theme.accent : theme.border, D(2));
            if (check.checked) UiDrawGlyph(dc, cb, RGB(255, 255, 255), UiGlyph::Check);
        }
    }

    const auto lo = std::min(selectionStart_, selectionEnd_), hi = std::max(selectionStart_, selectionEnd_);
    for (size_t pi = 0; pi < message.paragraphs.size(); ++pi) {
        const auto &p = message.paragraphs[pi];
        if (p.rect.bottom + box.top <= viewport.top) continue;
        if (p.rect.top + box.top >= viewport.bottom) break;
        const auto *block = p.block < message.blocks.size() ? &message.blocks[p.block] : nullptr;
        const auto shift = block ? block->offset : 0;
        auto area = viewport;
        if (block) {
            const auto body = Moved(block->body, box.left, box.top);
            if (!IntersectRect(&area, &area, &body)) continue;
        }
        const bool alertHeading = block && block->kind == Kind::Quote && block->alert != AlertKind::None &&
                                  p.style == 3 && !user;
        const COLORREF alertInk = alertHeading ? AlertStyle(block->alert, dark_, plain).second : plain;
        for (const auto &line : p.lines) {
            auto row = Moved(line.rect, box.left - shift, box.top);
            auto clip = row;
            if (!IntersectRect(&clip, &clip, &area)) continue;
            if (!line.length) continue;

            const auto xBegin = std::upper_bound(line.advances.begin(), line.advances.end(), clip.left - row.left);
            size_t start = line.start + static_cast<size_t>(xBegin - line.advances.begin());
            if (start > line.start) --start;
            if (start < p.text.size() && start > line.start && LowSurrogate(p.text[start])) --start;
            const auto xEnd = std::lower_bound(line.advances.begin(), line.advances.end(), clip.right - row.left);
            size_t end = line.start + std::min(line.length, static_cast<size_t>(xEnd - line.advances.begin()));
            if (end < line.start + line.length && LowSurrogate(p.text[end])) ++end;

            // Pre-pass for <mark> and <kbd> inline backgrounds before selection fill covers them.
            if (p.style != 2) {
                auto bgSpan = std::lower_bound(p.spans.begin(), p.spans.end(), start, [](const Span &value, size_t at) {
                    return value.start + value.length <= at;
                });
                for (; bgSpan != p.spans.end() && bgSpan->start < end; ++bgSpan) {
                    const size_t b0 = std::max(start, bgSpan->start);
                    const size_t b1 = std::min(end, bgSpan->start + bgSpan->length);
                    if (b1 <= b0) continue;
                    const int x0 = row.left + line.advances[b0 - line.start];
                    const int x1 = row.left + line.advances[b1 - line.start];
                    const bool isKbd = (bgSpan->flags & ai::markup::KbdFlag) || bgSpan->ink == Ink::Kbd;
                    const bool isInlineCode = (bgSpan->flags & ai::markup::InlineCodeFlag) != 0 ||
                                              bgSpan->ink == Ink::InlineCode;
                    if (isKbd || isInlineCode) {
                        const int padX = D(4);
                        const int boxX0 = x0 - padX;
                        const bool atSpanEnd = (b1 == bgSpan->start + bgSpan->length);
                        const int boxX1 = x1 + (atSpanEnd ? -D(2) : padX);
                        if (isKbd) {
                            DrawBoxClipped(dc, {boxX0, row.top + D(2), boxX1, row.bottom - D(2)}, clip,
                                           user ? UiAccentInset() : theme.raised, user ? plain : border, D(3));
                        } else {
                            const COLORREF codeFill = user ? UiAccentInset() : (dark_ ? RGB(40, 45, 54) : RGB(236, 240, 246));
                            DrawBoxClipped(dc, {boxX0, row.top + D(2), boxX1, row.bottom - D(2)}, clip,
                                           codeFill, user ? codeFill : border, D(4));
                        }
                    } else if ((bgSpan->flags & ai::markup::MarkFlag) || bgSpan->ink == Ink::Mark) {
                        const COLORREF markFill = user ? UiAccentInset()
                            : (dark_ ? RGB(92, 74, 22) : RGB(255, 242, 166));
                        RECT markRect{x0, row.top + D(1), x1, row.bottom - D(1)};
                        if (IntersectRect(&markRect, &markRect, &clip)) Fill(dc, markRect, markFill);
                    }
                }
            }

            const auto first = TextPoint{index, pi, line.start};
            const auto last = TextPoint{index, pi, line.start + line.length};
            const bool selection = lo != hi && std::get<0>(lo) != SIZE_MAX && hi > first && lo < last;
            size_t selectedStart = line.start, selectedEnd = line.start;
            RECT selected{};
            if (selection) {
                if (std::get<0>(lo) == index && std::get<1>(lo) == pi) selectedStart = std::max(line.start, std::get<2>(lo));
                selectedEnd = line.start + line.length;
                if (std::get<0>(hi) == index && std::get<1>(hi) == pi) selectedEnd = std::min(selectedEnd, std::get<2>(hi));
                selected = {row.left + line.advances[selectedStart - line.start], row.top,
                            row.left + line.advances[selectedEnd - line.start], row.bottom};
                if (IntersectRect(&selected, &selected, &clip)) Fill(dc, selected, user ? RGB(15, 36, 102) : theme.accent);
            }

            const auto draw = [&](size_t begin, size_t finish, Ink ink, unsigned flags) {
                if (finish <= begin) return;
                SelectObject(dc, ParagraphFont(p.style, ink, flags));
                const COLORREF normalColor = alertHeading ? alertInk
                    : SyntaxInk(ink, flags, dark_, plain, mutedInk, user, p.muted);
                const int subShift = p.style == 1 ? D(6) : p.style == 4 ? D(5) : p.style == 5 ? D(4) : D(3);
                const int supShift = p.style == 1 ? 0 : -D(2);
                int yShift = (flags & ai::markup::SubFlag) || ink == Ink::Sub ? subShift
                    : ((flags & ai::markup::SupFlag) || ink == Ink::Sup ? supShift : 0);
                if (yShift == 0 && p.style != 2 &&
                    ((flags & (ai::markup::InlineCodeFlag | ai::markup::KbdFlag)) != 0 ||
                     ink == Ink::InlineCode || ink == Ink::Kbd)) {
                    TEXTMETRICW codeTm{};
                    if (GetTextMetricsW(dc, &codeTm)) {
                        const int pillInnerH = std::max(1, static_cast<int>(row.bottom - row.top) - D(4));
                        yShift = D(2) + (pillInnerH - static_cast<int>(codeTm.tmHeight)) / 2;
                    }
                }
                const bool drawUnder = (flags & (ai::markup::UnderlineFlag | ai::markup::LinkFlag)) != 0 ||
                                       ink == Ink::Underline || ink == Ink::Link;
                const bool drawStrike = (flags & ai::markup::StrikeFlag) != 0 || ink == Ink::Strike;
                const auto drawGlyphs = [&](const RECT &bounds, COLORREF textColor) {
                    SetTextColor(dc, textColor);
                    std::vector<int> dx;
                    for (size_t offset = begin; offset < finish;) {
                        if (p.text[offset] == L'\t' || p.text[offset] == L'\n') { ++offset; continue; }
                        const auto stopIt = std::find_if(p.text.begin() + offset, p.text.begin() + finish,
                                                         [](wchar_t c) { return c == L'\t' || c == L'\n'; });
                        const auto stop = static_cast<size_t>(stopIt - p.text.begin());
                        dx.clear();
                        dx.reserve(stop - offset);
                        for (size_t k = offset; k < stop; ++k) {
                            dx.push_back(line.advances[k + 1 - line.start] - line.advances[k - line.start]);
                        }
                        ExtTextOutW(dc, row.left + line.advances[offset - line.start], row.top + yShift, ETO_CLIPPED,
                                    &bounds, p.text.data() + offset, static_cast<UINT>(stop - offset),
                                    dx.empty() ? nullptr : dx.data());
                        offset = stop;
                    }
                    if (drawUnder || drawStrike) {
                        const int spanX0 = std::max<int>(bounds.left, row.left + line.advances[begin - line.start]);
                        const int spanX1 = std::min<int>(bounds.right, row.left + line.advances[finish - line.start]);
                        if (spanX1 > spanX0) {
                            if (drawUnder) {
                                const int uy = std::min(static_cast<int>(row.bottom) - 1, static_cast<int>(row.bottom) - D(3));
                                Fill(dc, {spanX0, uy, spanX1, uy + 1}, textColor);
                            }
                            if (drawStrike) {
                                const int sy = (row.top + row.bottom) / 2;
                                Fill(dc, {spanX0, sy, spanX1, sy + 1}, textColor);
                            }
                        }
                    }
                };
                drawGlyphs(clip, normalColor);
                if (selected.right > selected.left && begin < selectedEnd && finish > selectedStart) {
                    drawGlyphs(selected, RGB(255, 255, 255));
                }
            };
            auto position = start;
            auto span = std::lower_bound(p.spans.begin(), p.spans.end(), start, [](const Span &value, size_t at) {
                return value.start + value.length <= at;
            });
            for (; span != p.spans.end() && span->start < end; ++span) {
                draw(position, std::min(end, span->start), Ink::Plain, 0);
                const auto finish = std::min(end, span->start + span->length);
                draw(std::max(position, span->start), finish, span->ink, span->flags);
                position = finish;
            }
            draw(position, end, Ink::Plain, 0);
        }
    }
    if (originalFont) SelectObject(dc, originalFont);
}

bool AiPane::TranscriptBlockPress(POINT point) {
    for (size_t i = 0; i < messages_.size(); ++i) {
        auto &message = messages_[i];
        if (message.hidden || message.folded || !PtInRect(&message.bounds, point)) continue;
        const POINT local{point.x - message.bounds.left, point.y - message.bounds.top};
        for (size_t b = 0; b < message.blocks.size(); ++b) {
            auto &block = message.blocks[b];
            if (block.kind == Kind::Code && PtInRect(&block.copy, local)) {
                selectedMessage_ = i;
                ClearTextSelection();
                Copy(ReplyCodeText(message, block), L"已复制代码");
                return true;
            }
            if (block.kind == Kind::Details && PtInRect(&block.toggle, local)) {
                selectedMessage_ = i;
                if (message.toggledDetails.count(block.source)) message.toggledDetails.erase(block.source);
                else message.toggledDetails.insert(block.source);
                message.measuredWidth = 0;
                ClearTextSelection();
                Reflow();
                return true;
            }
            if (block.maximum && PtInRect(&block.track, local)) {
                const auto thumb = ReplyScrollThumb(block);
                if (PtInRect(&thumb, local)) {
                    draggedBlockMessage_ = i;
                    draggedBlock_ = b;
                    blockThumbGrab_ = local.x - thumb.left;
                    SetCapture(transcript_);
                } else {
                    const int page = block.body.right - block.body.left;
                    block.offset = std::clamp(block.offset + (local.x < thumb.left ? -page : page), 0, block.maximum);
                    InvalidateRect(transcript_, nullptr, FALSE);
                }
                return true;
            }
        }
        break;
    }
    return false;
}

std::wstring AiPane::ReplyCodeText(const Message &message, const ReplyBlock &block) {
    return message.text.substr(block.codeBegin, block.codeEnd - block.codeBegin);
}

bool AiPane::TranscriptHorizontal(POINT point, int pixels) {
    point.y += scroll_.offset;
    for (auto &message : messages_) {
        if (message.hidden || message.folded || !PtInRect(&message.bounds, point)) continue;
        const POINT local{point.x - message.bounds.left, point.y - message.bounds.top};
        for (auto &block : message.blocks) {
            if (!block.maximum || !PtInRect(&block.rect, local)) continue;
            block.offset = std::clamp(block.offset + pixels, 0, block.maximum);
            InvalidateRect(transcript_, nullptr, FALSE);
            return true;
        }
        break;
    }
    return false;
}

void AiPane::DragTranscriptBlock(int x) {
    if (draggedBlockMessage_ >= messages_.size()) return;
    auto &message = messages_[draggedBlockMessage_];
    if (message.hidden || message.folded || draggedBlock_ >= message.blocks.size()) return;
    auto &block = message.blocks[draggedBlock_];
    const auto thumb = ReplyScrollThumb(block);
    const int range = std::max(1L, block.track.right - block.track.left - (thumb.right - thumb.left));
    const auto position = std::clamp(x - message.bounds.left - block.track.left - blockThumbGrab_, 0L, static_cast<long>(range));
    block.offset = MulDiv(position, block.maximum, range);
    InvalidateRect(transcript_, nullptr, FALSE);
}

std::wstring AiPane::HitLink(POINT point) const {
    point.y += scroll_.offset;
    for (const auto &m : messages_) {
        if (m.hidden || m.folded || !PtInRect(&m.bounds, point)) continue;
        const int rawX = point.x - m.bounds.left;
        const int localY = point.y - m.bounds.top;
        for (const auto &p : m.paragraphs) {
            if (p.links.empty() || localY < p.rect.top || localY >= p.rect.bottom) continue;
            const auto *block = p.block < m.blocks.size() ? &m.blocks[p.block] : nullptr;
            if (block && !PtInRect(&block->body, POINT{rawX, localY})) continue;
            const int localX = rawX + (block ? block->offset : 0);
            for (const auto &line : p.lines) {
                if (localY < line.rect.top || localY >= line.rect.bottom) continue;
                if (localX < line.rect.left || localX > line.rect.left + line.advances.back()) continue;
                const int relX = localX - line.rect.left;
                const auto upper = std::upper_bound(line.advances.begin(), line.advances.end(), relX);
                if (upper == line.advances.begin()) continue;
                const size_t charIndex = std::min(line.length ? line.length - 1 : size_t(0),
                                                  static_cast<size_t>(upper - line.advances.begin() - 1));
                const size_t offset = line.start + charIndex;
                for (const auto &link : p.links) {
                    if (offset >= link.start && offset < link.start + link.length) return link.url;
                }
            }
        }
        break;
    }
    return {};
}

bool AiPane::ActivateLink(const std::wstring &url) {
    if (url.empty()) return false;
    if (url.front() == L'#') {
        const auto targetSlug = AnchorSlug(std::wstring_view(url).substr(1));
        if (targetSlug.empty()) return false;
        for (const auto &m : messages_) {
            if (m.hidden || m.folded) continue;
            for (const auto &p : m.paragraphs) {
                if (p.headingLevel <= 0) continue;
                if (AnchorSlug(p.text) == targetSlug) {
                    scroll_.offset = std::clamp(static_cast<int>(m.bounds.top + p.rect.top - D(8)), 0, scroll_.Maximum());
                    InvalidateRect(transcript_, nullptr, FALSE);
                    UpdateTranscriptActions();
                    return true;
                }
            }
        }
        return false;
    }
    if (url.rfind(L"serialctl-exec://", 0) == 0) {
        const std::string execId = WideToMultiByte(url.substr(17), CP_UTF8);
        if (!execId.empty() && sidebarCallbacks_.onFocusExec)
            sidebarCallbacks_.onFocusExec(execId);
        return !execId.empty();
    }
    if (url.rfind(L"serialctl-change://", 0) == 0) {
        const std::string changeId = WideToMultiByte(url.substr(19), CP_UTF8);
        if (!changeId.empty() && sidebarCallbacks_.onFileChangesUpdated)
            sidebarCallbacks_.onFileChangesUpdated(fileChanges_, turnSeq_, changeId, true);
        return !changeId.empty();
    }
    if (url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0 || url.rfind(L"mailto:", 0) == 0) {
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    return false;
}

} // namespace serialctl
