#include "AiPane.h"
#include <numeric>

namespace serialctl {
namespace {
using ai::markup::Alignment;
using ai::markup::Ink;
using ai::markup::Kind;
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
COLORREF SyntaxInk(Ink ink, bool dark, COLORREF plain) {
    switch (ink) {
    case Ink::Keyword: return dark ? RGB(202, 168, 251) : RGB(106, 53, 157);
    case Ink::String:  return dark ? RGB(164, 216, 157) : RGB(27, 103, 50);
    case Ink::Number:  return dark ? RGB(255, 185, 142) : RGB(146, 61, 24);
    case Ink::Comment: return dark ? RGB(166, 173, 180) : RGB(96, 98, 106);
    case Ink::Name:    return dark ? RGB(142, 204, 244) : RGB(21, 84, 140);
    default: return plain;
    }
}
bool LowSurrogate(wchar_t c) { return c >= 0xdc00 && c <= 0xdfff; }

// Keep one measurement and one source index per UTF-16 code unit. The same
// advances drive wrapping, clipping, selection and horizontal hit testing.
void MeasureRun(HDC dc, const std::wstring &text, size_t start, size_t end, std::vector<int> &advances) {
    if (start >= end) return;
    SIZE blank{};
    GetTextExtentPoint32W(dc, L" ", 1, &blank);
    const int tab = std::max(1L, blank.cx * 4);
    int widths[512];
    while (start < end) {
        if (text[start] == L'\t') {
            advances[start + 1] = (advances[start] / tab + 1) * tab;
            ++start;
            continue;
        }
        size_t count = 1;
        while (count < std::size(widths) && start + count < end && text[start + count] != L'\t') ++count;
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

} // namespace

HFONT AiPane::ParagraphFont(int style, Ink ink) const {
    if (style == 2 || ink == Ink::InlineCode) return codeFont_;
    if (style == 1) return titleFont_;
    if (style == 3 || ink == Ink::Bold) return boldFont_ ? boldFont_ : font_;
    return font_;
}

void AiPane::LayoutMessage(HDC dc, Message &message, int width) {
    const auto previous = std::move(message.blocks);
    message.paragraphs.clear();
    message.blocks.clear();
    const bool user = message.role == L"我";
    auto parsed = ai::markup::Parse(message.text, user || message.role == L"AI");
    const auto originalFont = GetCurrentObject(dc, OBJ_FONT);
    int y = D(12) + (message.groupHeader ? D(28) : 0);
    const int margin = D(16), right = width - margin;
    const auto measure = [&](const std::wstring &text, const std::vector<Span> &spans, int style, size_t limit) {
        std::vector<int> advances(limit + 1);
        size_t end = 0;
        if (style != 2) {
            for (const auto &span : spans) {
                if (span.start >= limit) break;
                SelectObject(dc, ParagraphFont(style));
                MeasureRun(dc, text, end, span.start, advances);
                SelectObject(dc, ParagraphFont(style, span.ink));
                end = std::min(limit, span.start + span.length);
                MeasureRun(dc, text, span.start, end, advances);
            }
        }
        SelectObject(dc, ParagraphFont(style));
        MeasureRun(dc, text, end, limit, advances);
        return advances;
    };

    auto paragraph = [&](Text text, int style, int left, int edge, int top, bool wrap,
                         size_t block = SIZE_MAX, Alignment alignment = Alignment::Left, bool tabAfter = false) {
        Paragraph item{{left, top, edge, top}, std::move(text.value), style, {}, std::move(text.spans), block, tabAfter};
        auto advances = measure(item.text, item.spans, style, item.text.size());
        SelectObject(dc, ParagraphFont(style));
        TEXTMETRICW metric{};
        GetTextMetricsW(dc, &metric);
        const int lineHeight = style == 0 && item.text.empty() && block == SIZE_MAX
            ? D(8) : std::max(D(20), static_cast<int>(metric.tmHeight) + D(4));
        const int available = std::max(1, edge - left);
        size_t offset = 0;
        do {
            auto next = item.text.size();
            if (wrap && advances.back() - advances[offset] > available) {
                const auto fit = std::upper_bound(advances.begin() + offset, advances.end(), advances[offset] + available);
                next = std::max(offset + 1, static_cast<size_t>(fit - advances.begin() - 1));
                if (next < item.text.size() && LowSurrogate(item.text[next]))
                    next = next == offset + 1 ? next + 1 : next - 1;
                const auto half = offset + (next - offset) / 2;
                size_t space = next;
                while (space > half && item.text[space - 1] != L' ' && item.text[space - 1] != L'\t') --space;
                if (space > half) next = space;
            }
            const auto textWidth = advances[next] - advances[offset];
            const auto remaining = std::max(0, available - textWidth);
            const auto shift = alignment == Alignment::Right ? remaining : alignment == Alignment::Center ? remaining / 2 : 0;
            TextLine line{{left + shift, top, wrap ? edge : left + std::max(available, textWidth), top + lineHeight},
                          offset, next - offset, {}};
            if (!wrap) {
                line.advances = std::move(advances);
            } else {
                line.advances.reserve(line.length + 1);
                for (auto i = offset; i <= next; ++i) line.advances.push_back(advances[i] - advances[offset]);
            }
            item.lines.push_back(std::move(line));
            top += lineHeight;
            offset = next;
        } while (offset < item.text.size());
        item.rect.bottom = top;
        message.paragraphs.push_back(std::move(item));
        return top;
    };
    const auto preferredWidth = [&](const Text &text, int style) {
        size_t limit = std::min<size_t>(text.value.size(), 160);
        if (limit < text.value.size() && LowSurrogate(text.value[limit])) --limit;
        return measure(text.value, text.spans, style, limit).back();
    };

    for (auto &block : parsed) {
        if (block.kind == Kind::Prose || block.kind == Kind::Heading || block.kind == Kind::List) {
            const auto style = block.kind == Kind::Heading ? 1 : 0;
            if (style) y += D(4);
            for (auto &line : block.lines)
                y = paragraph(std::move(line), style, margin + (block.kind == Kind::List ? D(6) : 0), right, y, true);
            y += D(4);
            continue;
        }
        const auto index = message.blocks.size();
        ReplyBlock visual;
        visual.kind = block.kind;
        visual.source = block.source;
        const auto old = std::lower_bound(previous.begin(), previous.end(), block.source,
            [](const ReplyBlock &candidate, size_t source) { return candidate.source < source; });
        const bool existed = old != previous.end() && old->source == block.source && old->kind == block.kind;
        visual.rect = {margin, y, right, y};
        if (block.kind == Kind::Rule) {
            visual.rect.top += D(8);
            visual.rect.bottom = visual.rect.top + 1;
            y += D(16);
        } else if (block.kind == Kind::Code) {
            visual.language = block.language.empty() ? L"代码" : std::move(block.language);
            visual.codeBegin = block.codeBegin;
            visual.codeEnd = block.codeEnd;
            visual.copy = {right - D(66), y + D(2), right - D(4), y + D(30)};
            y += D(36);
            visual.body = {margin + D(10), y, right - D(10), y};
            int widest = 0;
            for (auto &line : block.lines) {
                y = paragraph(std::move(line), 2, visual.body.left, visual.body.right, y, false, index);
                widest = std::max(widest, message.paragraphs.back().lines.front().advances.back());
            }
            if (block.lines.empty()) y += D(20);
            visual.body.bottom = y;
            visual.maximum = std::max(0, widest - static_cast<int>(visual.body.right - visual.body.left));
            y += D(8);
        } else {
            const size_t count = block.alignment.size();
            const int available = std::max(1, right - margin);
            const int padding = D(10);
            if (existed && old->rows.size() > 1 && old->columns.size() == count && message.measuredWidth == width) {
                visual.columns = old->columns;
            } else {
                visual.columns.resize(count);
                std::vector<int> wanted(count);
                for (size_t column = 0; column < count; ++column) {
                    const auto header = preferredWidth(block.rows.front()[column], 3) + padding * 2 + D(2);
                    visual.columns[column] = std::clamp(header, D(64), D(132));
                    wanted[column] = std::min(D(480), std::max(header, visual.columns[column]));
                    for (size_t row = 1; row < std::min<size_t>(block.rows.size(), 12); ++row) {
                        const auto cellWidth = preferredWidth(block.rows[row][column], 0) + padding * 2 + D(2);
                        wanted[column] = std::max(wanted[column], std::min(D(480), cellWidth));
                    }
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
            visual.rect.right = margin + std::min(total, available);
            visual.body = visual.rect;
            for (size_t row = 0; row < block.rows.size(); ++row) {
                int x = margin;
                int bottom = y;
                const auto first = message.paragraphs.size();
                for (size_t column = 0; column < count; ++column) {
                    const int edge = x + visual.columns[column];
                    bottom = std::max(bottom, paragraph(std::move(block.rows[row][column]), row == 0 ? 3 : 0,
                        x + padding, edge - padding, y + D(6), true, index, block.alignment[column], column + 1 < count));
                    x = edge;
                }
                y = bottom + D(6);
                for (size_t p = first; p < message.paragraphs.size(); ++p) message.paragraphs[p].rect.bottom = y;
                visual.rows.push_back(y);
            }
            visual.body.bottom = y;
            visual.maximum = std::max(0, total - available);
        }
        if (visual.maximum) {
            visual.track = {visual.rect.left + D(6), y, visual.rect.right - D(6), y + D(14)};
            if (existed) visual.offset = std::min(visual.maximum, old->offset);
            y += D(14);
        }
        if (block.kind != Kind::Rule) visual.rect.bottom = y;
        message.blocks.push_back(std::move(visual));
        y += D(6);
    }
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
        for (const auto &line : p.lines) {
            auto row = Moved(line.rect, box.left - shift, box.top);
            auto clip = row;
            if (!IntersectRect(&clip, &clip, &area)) continue;
            if (!line.length) continue;
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

            const auto xBegin = std::upper_bound(line.advances.begin(), line.advances.end(), clip.left - row.left);
            size_t start = line.start + static_cast<size_t>(xBegin - line.advances.begin());
            if (start > line.start) --start;
            if (start < p.text.size() && start > line.start && LowSurrogate(p.text[start])) --start;
            const auto xEnd = std::lower_bound(line.advances.begin(), line.advances.end(), clip.right - row.left);
            size_t end = line.start + std::min(line.length, static_cast<size_t>(xEnd - line.advances.begin()));
            if (end < line.start + line.length && LowSurrogate(p.text[end])) ++end;
            const auto draw = [&](size_t begin, size_t finish, Ink ink) {
                if (finish <= begin) return;
                SelectObject(dc, ParagraphFont(p.style, ink));
                SetTextColor(dc, user ? plain : SyntaxInk(ink, dark_, plain));
                const auto drawGlyphs = [&](const RECT &bounds) {
                    for (size_t offset = begin; offset < finish;) {
                        if (p.text[offset] == L'\t') { ++offset; continue; }
                        const auto tab = std::find(p.text.begin() + offset, p.text.begin() + finish, L'\t');
                        const auto stop = static_cast<size_t>(tab - p.text.begin());
                        ExtTextOutW(dc, row.left + line.advances[offset - line.start], row.top, ETO_CLIPPED, &bounds,
                                    p.text.data() + offset, static_cast<UINT>(stop - offset), nullptr);
                        offset = stop;
                    }
                };
                drawGlyphs(clip);
                if (selected.right > selected.left && begin < selectedEnd && finish > selectedStart) {
                    SetTextColor(dc, RGB(255, 255, 255));
                    drawGlyphs(selected);
                }
            };
            auto position = start;
            auto span = std::lower_bound(p.spans.begin(), p.spans.end(), start, [](const Span &value, size_t at) {
                return value.start + value.length <= at;
            });
            for (; span != p.spans.end() && span->start < end; ++span) {
                draw(position, std::min(end, span->start), Ink::Plain);
                const auto finish = std::min(end, span->start + span->length);
                draw(std::max(position, span->start), finish, span->ink);
                position = finish;
            }
            draw(position, end, Ink::Plain);
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

} // namespace serialctl
