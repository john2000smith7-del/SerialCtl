#include "TerminalViewport.h"
#include "SerialShortcut.h"
#include "TerminalViewportGeometry.h"
#include "PerformanceMetrics.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <windowsx.h>
#include <algorithm>
namespace serialctl {
namespace {
bool PointInRect(const RECT& rect, POINT point) { return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom; }
COLORREF IndexedTerminalColor(int index, bool dark) {
    static const std::array<COLORREF, 16> DarkColors = {
        RGB(20, 20, 22), RGB(255, 95, 86), RGB(48, 209, 88), RGB(255, 214, 10),
        RGB(64, 156, 255), RGB(191, 90, 242), RGB(100, 210, 255), RGB(229, 229, 234),
        RGB(99, 99, 102), RGB(255, 105, 97), RGB(78, 230, 111), RGB(255, 224, 69),
        RGB(94, 176, 255), RGB(218, 143, 255), RGB(112, 215, 255), RGB(255, 255, 255)};
    static const std::array<COLORREF, 16> LightColors = {
        RGB(29, 29, 31), RGB(198, 40, 40), RGB(24, 128, 56), RGB(151, 112, 0),
        RGB(0, 102, 204), RGB(137, 49, 178), RGB(0, 125, 140), RGB(205, 205, 210),
        RGB(99, 99, 102), RGB(215, 54, 54), RGB(35, 150, 70), RGB(176, 128, 0),
        RGB(0, 122, 255), RGB(175, 82, 222), RGB(0, 145, 165), RGB(255, 255, 255)};
    index = std::max(0, std::min(255, index));
    if (index < 16) return dark ? DarkColors[static_cast<size_t>(index)] : LightColors[static_cast<size_t>(index)];
    if (index < 232) {
        const int value = index - 16;
        const int blue = value % 6;
        const int green = value / 6 % 6;
        const int red = value / 36;
        const auto component = [](int level) { return level == 0 ? 0 : 55 + level * 40; };
        return RGB(component(red), component(green), component(blue));
    }
    const int gray = 8 + (index - 232) * 10;
    return RGB(gray, gray, gray);
}

COLORREF ResolveTerminalColor(
    const TerminalColor& color, COLORREF fallback, bool dark, bool bold) {
    if (color.kind == TerminalColor::Kind::Rgb)
        return RGB(color.red, color.green, color.blue);
    if (color.kind == TerminalColor::Kind::Indexed) {
        int index = color.index;
        if (bold && index < 8) index += 8;
        return IndexedTerminalColor(index, dark);
    }
    return fallback;
}


void DrawTextSimple(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SelectObject(dc, oldFont);
}
void DrawRoundedBox(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) { UiBox(dc, rect, fill, border, radius); }

void DrawRegisterPreview(HDC dc, RECT client, const RECT& selection, std::uint32_t value,
                         const TerminalViewport::Options& options,
                         RECT* cardBounds, std::array<RECT, 32>* bitBounds) {
    const int pad = Ui::Scale(10), gap = Ui::Scale(6);
    const int cellWidth = Ui::Scale(20), groupPad = Ui::Scale(3);
    const int groupWidth = 8 * cellWidth + 2 * groupPad;
    const int groupHeight = Ui::Scale(39), titleHeight = Ui::Scale(21), headerHeight = 2 * titleHeight;
    client.left += gap;
    client.right -= Ui::OverlayScrollLaneWidth + gap;
    client.top += gap;
    client.bottom -= gap;
    const int columns = client.right - client.left >= 2 * (pad + groupWidth) + gap ? 2 : 1;
    const int rows = 4 / columns;
    const int width = 2 * pad + columns * groupWidth + (columns - 1) * gap;
    const int height = pad + headerHeight + rows * groupHeight + (rows - 1) * gap;
    if (width > client.right - client.left || height > client.bottom - client.top) return;

    LONG left = std::clamp(selection.left, client.left, client.right - width);
    LONG top = selection.bottom + gap;
    if (top + height > client.bottom) {
        top = selection.top - gap - height;
        if (top < client.top) {
            if (selection.right + gap + width <= client.right) left = selection.right + gap;
            else if (selection.left - gap - width >= client.left) left = selection.left - gap - width;
            else return;
            top = std::clamp(selection.top, client.top, client.bottom - height);
        }
    }
    const RECT card{left, top, left + width, top + height};
    if (cardBounds) *cardBounds = card;
    const auto colors = UiTheme(options.dark);
    UiBox(dc, card, colors.surface, colors.border, Ui::CardRadius);
    wchar_t hex[11]{};
    swprintf_s(hex, L"0x%08X", static_cast<unsigned>(value));
    const RECT title{card.left + pad, card.top, card.right - pad, card.top + titleHeight};
    DrawTextSimple(dc, hex, title, colors.text, options.smallFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextSimple(dc, L"32 位", title, colors.muted, options.smallFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    const RECT decimal{title.left, title.bottom, title.right, title.bottom + titleHeight};
    DrawTextSimple(dc, L"十进制  " + std::to_wstring(value), decimal, colors.text, options.smallFont,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (int group = 0; group < 4; ++group) {
        const LONG x = card.left + pad + (group % columns) * (groupWidth + gap);
        const LONG y = card.top + headerHeight + (group / columns) * (groupHeight + gap);
        UiBox(dc, {x, y, x + groupWidth, y + groupHeight}, colors.raised, colors.raised, Ui::Scale(6));
        for (int offset = 0; offset < 8; ++offset) {
            const int bit = 31 - group * 8 - offset;
            const bool set = (value & (std::uint32_t{1} << bit)) != 0;
            const LONG bitX = x + groupPad + offset * cellWidth;
            const RECT number{bitX, y + Ui::Scale(2), bitX + cellWidth, y + Ui::Scale(18)};
            const RECT digit{bitX, number.bottom, bitX + cellWidth, y + groupHeight - Ui::Scale(3)};
            if (bitBounds) (*bitBounds)[static_cast<size_t>(bit)] =
                {number.left, number.top, digit.right, digit.bottom};
            if (set) UiBox(dc, {digit.left + 1, digit.top, digit.right - 1, digit.bottom},
                          colors.accentSoft, colors.accentSoft, Ui::Scale(4));
            DrawTextSimple(dc, std::to_wstring(bit), number, colors.muted, options.smallFont,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextSimple(dc, set ? L"1" : L"0", digit, set ? UiAccentInk(options.dark) : colors.text,
                           options.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}
}

void TerminalViewport::ShowRegisterPreview(HWND window, std::wstring_view selection) {
    const auto value = TerminalViewportController::ParseRegisterValue(selection);
    if (!value) {
        DismissRegisterPreview(window);
        return;
    }
    registerPreview_ = RegisterPreview{*value, std::wstring(selection)};
    registerCardBounds_ = {};
    registerBitBounds_.fill(RECT{});
    cache_.valid = false;
    InvalidateRect(window, nullptr, FALSE);
}

void TerminalViewport::DismissRegisterPreview(HWND window) {
    if (!registerPreview_) return;
    registerPreview_.reset();
    registerCardBounds_ = {};
    registerBitBounds_.fill(RECT{});
    cache_.valid = false;
    if (window) InvalidateRect(window, nullptr, FALSE);
}

namespace {
struct FrameKey {
    const TerminalViewport::State* state = nullptr;
    std::uint64_t revision = 0;
    int width = 0, height = 0, scroll = 0, horizontal = 0;
    size_t anchorLine = 0, focusLine = 0;
    int anchorColumn = 0, focusColumn = 0;
    bool selection = false, newOutput = false, focused = false;
    bool dark = false, timestamps = false, liveMonitor = false;
    int mode = -1;
    HFONT font = nullptr, smallFont = nullptr;
    std::wstring draft;
    size_t draftCursor = 0;
    bool operator==(const FrameKey& other) const {
        return state == other.state && revision == other.revision &&
               width == other.width && height == other.height &&
               scroll == other.scroll && horizontal == other.horizontal &&
               anchorLine == other.anchorLine && focusLine == other.focusLine &&
               anchorColumn == other.anchorColumn && focusColumn == other.focusColumn &&
               selection == other.selection && newOutput == other.newOutput &&
               focused == other.focused && dark == other.dark &&
               timestamps == other.timestamps && liveMonitor == other.liveMonitor &&
               mode == other.mode && font == other.font && smallFont == other.smallFont &&
               draft == other.draft && draftCursor == other.draftCursor;
    }
};

struct RenderFrameParams {
    int width = 0, height = 0;
    int padding = 0, textLeft = 0, cellWidth = 8, lineHeight = 16, columns = 80, rows = 24;
    int horizontalOffset = 0, scrollOffset = 0;
    size_t topLine = 0, totalLines = 0;
    size_t selectionStartLine = 0, selectionEndLine = 0;
    int selectionStartColumn = 0, selectionEndColumn = 0;
    bool dark = false, showTimestamps = false, selectionActive = false;
    bool drawCursor = false, showNewOutput = false;
    size_t cursorLine = 0;
    int cursorColumn = 0;
    bool vScrollVisible = false, hScrollVisible = false;
    bool liveMonitor = false;
    RECT vThumb{}, hThumb{};
    HFONT font = nullptr, smallFont = nullptr;
    int radius3 = 3, radiusCard = 6, outerRadius = 8;
};

template <typename LineAt>
void RenderTerminalSurface(HDC buffer, const RECT& client, const RenderFrameParams& p,
                           const LineAt& lineAt, std::uint64_t& gdiCalls) {
    const auto colors = UiTheme(p.dark);
    HBRUSH dcBrush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    int savedCard = 0;
    if (p.liveMonitor) {
        // Fill outer tray background matching rightCard and clip content inside the rounded card
        // so both Light and Dark modes frame the terminal cleanly without exposed sharp corners.
        const COLORREF trayBg = p.dark ? colors.field : colors.window;
        SetDCBrushColor(buffer, trayBg);
        FillRect(buffer, &client, dcBrush);
        UiBox(buffer, client, colors.field, colors.border, p.outerRadius);
        savedCard = SaveDC(buffer);
        const int r = std::max(0, p.outerRadius);
        if (HRGN rgn = CreateRoundRectRgn(client.left + 1, client.top + 1, client.right, client.bottom, r * 2, r * 2)) {
            SelectClipRgn(buffer, rgn);
            DeleteObject(rgn);
        }
        gdiCalls = 2;
    } else {
        SetDCBrushColor(buffer, colors.field);
        FillRect(buffer, &client, dcBrush);
        gdiCalls = 1;
    }

    if (p.totalLines > 0 || p.drawCursor) {
        HFONT previousFont = reinterpret_cast<HFONT>(SelectObject(buffer, p.font));
        SetBkMode(buffer, TRANSPARENT);
        const int availableColumns = std::max(0,
            (static_cast<int>(client.right) - p.textLeft - Ui::OverlayScrollLaneWidth) /
                std::max(1, p.cellWidth));
        std::wstring text;
        std::vector<int> advances;
        text.reserve(static_cast<size_t>(availableColumns));
        advances.reserve(static_cast<size_t>(availableColumns));

        for (int viewRow = 0; viewRow < p.rows; ++viewRow) {
            const size_t lineIndex = p.topLine + static_cast<size_t>(viewRow);
            if (lineIndex >= p.totalLines) break;
            const TerminalLine& line = lineAt(lineIndex);
            const int y = p.padding + viewRow * p.lineHeight;
            if (p.showTimestamps && !line.timestamp.empty()) {
                RECT timestampRect{p.padding, y, p.textLeft - Ui::Gap, y + p.lineHeight};
                DrawTextSimple(buffer, line.timestamp, timestampRect, colors.muted,
                    p.smallFont, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                ++gdiCalls;
            }

            int viewColumn = 0;
            while (viewColumn < availableColumns) {
                const int column = viewColumn + p.horizontalOffset;
                if (column < 0 || static_cast<size_t>(column) >= line.cells.size()) break;
                const TerminalCell& cell = line.cells[static_cast<size_t>(column)];
                if (cell.continuation) { ++viewColumn; continue; }
                const auto visual = [&](const TerminalCell& value, int modelColumn) {
                    const bool selected = p.selectionActive &&
                        (lineIndex > p.selectionStartLine ||
                            (lineIndex == p.selectionStartLine && modelColumn >= p.selectionStartColumn)) &&
                        (lineIndex < p.selectionEndLine ||
                            (lineIndex == p.selectionEndLine && modelColumn <= p.selectionEndColumn));
                    COLORREF foreground = ResolveTerminalColor(value.attributes.foreground,
                        colors.text, p.dark, value.attributes.bold);
                    COLORREF background = ResolveTerminalColor(value.attributes.background,
                        colors.field, p.dark, false);
                    if (value.attributes.inverse) std::swap(foreground, background);
                    if (selected) { background = colors.accentSoft; foreground = colors.text; }
                    struct Visual { COLORREF foreground, background; bool bold, underline; };
                    return Visual{foreground, background, value.attributes.bold, value.attributes.underline};
                };
                const auto runVisual = visual(cell, column);
                const auto sameVisual = [](const auto& left, const auto& right) {
                    return left.foreground == right.foreground && left.background == right.background &&
                        left.bold == right.bold && left.underline == right.underline;
                };
                const int runStart = viewColumn;
                text.clear();
                advances.clear();
                while (viewColumn < availableColumns) {
                    const int runColumn = viewColumn + p.horizontalOffset;
                    if (runColumn < 0 || static_cast<size_t>(runColumn) >= line.cells.size()) break;
                    const auto& runCell = line.cells[static_cast<size_t>(runColumn)];
                    if (runCell.continuation) { ++viewColumn; continue; }
                    const auto currentVisual = visual(runCell, runColumn);
                    if (!sameVisual(runVisual, currentVisual)) break;
                    const int width = static_cast<size_t>(runColumn + 1) < line.cells.size() &&
                        line.cells[static_cast<size_t>(runColumn + 1)].continuation ? 2 : 1;
                    text.push_back(runCell.character);
                    advances.push_back(width * p.cellWidth);
                    viewColumn += width;
                }
                const int runRight = viewColumn;
                RECT runRect{p.textLeft + runStart * p.cellWidth, y,
                    p.textLeft + runRight * p.cellWidth, y + p.lineHeight};
                if (runVisual.background != colors.field) {
                    SetDCBrushColor(buffer, runVisual.background);
                    FillRect(buffer, &runRect, dcBrush);
                    ++gdiCalls;
                }
                if (!text.empty() && std::any_of(text.begin(), text.end(), [](wchar_t value) { return value != L' '; })) {
                    SetTextColor(buffer, runVisual.foreground);
                    ExtTextOutW(buffer, runRect.left, runRect.top, 0, nullptr, text.data(),
                                static_cast<UINT>(text.size()), advances.data());
                    ++gdiCalls;
                    if (runVisual.bold) {
                        ExtTextOutW(buffer, runRect.left + 1, runRect.top, 0, nullptr, text.data(),
                                    static_cast<UINT>(text.size()), advances.data());
                        ++gdiCalls;
                    }
                }
                if (runVisual.underline) {
                    HPEN dcPen = reinterpret_cast<HPEN>(GetStockObject(DC_PEN));
                    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(buffer, dcPen));
                    SetDCPenColor(buffer, runVisual.foreground);
                    MoveToEx(buffer, runRect.left, runRect.bottom - 2, nullptr);
                    LineTo(buffer, runRect.right, runRect.bottom - 2);
                    SelectObject(buffer, oldPen);
                    ++gdiCalls;
                }
            }
        }

        if (p.drawCursor &&
            p.cursorLine >= p.topLine &&
            p.cursorLine < p.topLine + static_cast<size_t>(p.rows) &&
            p.cursorColumn >= 0 && p.cursorColumn < p.columns) {
            const int viewRow = static_cast<int>(p.cursorLine - p.topLine);
            RECT cursor{p.textLeft + p.cursorColumn * p.cellWidth,
                p.padding + viewRow * p.lineHeight,
                p.textLeft + (p.cursorColumn + 1) * p.cellWidth,
                p.padding + (viewRow + 1) * p.lineHeight};
            RECT cursorLine{cursor.left, cursor.bottom - 2, cursor.right, cursor.bottom};
            SetDCBrushColor(buffer, colors.accent);
            FillRect(buffer, &cursorLine, dcBrush);
            ++gdiCalls;
        }
        SelectObject(buffer, previousFont);

        if (p.vScrollVisible) {
            DrawRoundedBox(buffer, p.vThumb, p.radius3, colors.border, colors.border);
            ++gdiCalls;
        }
        if (p.hScrollVisible) {
            DrawRoundedBox(buffer, p.hThumb, p.radius3, colors.border, colors.border);
            ++gdiCalls;
        }
        if (p.showNewOutput) {
            const RECT notification = TerminalNewOutputRect(client, p.hScrollVisible);
            DrawRoundedBox(buffer, notification, p.radiusCard, colors.accentSoft, colors.accent);
            DrawTextSimple(buffer, L"↓  新输出", notification, UiAccentInk(p.dark),
                p.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            gdiCalls += 2;
        }
    }
    if (p.liveMonitor && savedCard) {
        RestoreDC(buffer, savedCard);
        const float w = static_cast<float>(client.right - client.left - 1);
        const float h = static_cast<float>(client.bottom - client.top - 1);
        if (w > 0 && h > 0) {
            const float x = client.left + 0.5f, y = client.top + 0.5f;
            const float diameter = std::min(static_cast<float>(std::max(0, p.outerRadius) * 2), std::min(w, h));
            Gdiplus::Graphics graphics(buffer);
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
            Gdiplus::Pen borderPen(UiColor(colors.border), 1.0f);
            graphics.DrawPath(&borderPen, &path);
        }
    }
}
} // namespace

struct TerminalViewport::AsyncRasterizer {
    struct Job {
        FrameKey key;
        RenderFrameParams params;
        LOGFONTW fontLog{};
        LOGFONTW smallFontLog{};
        bool hasFontLog = false;
        bool hasSmallFontLog = false;
        std::vector<TerminalLine> visibleLines;
        HWND notifyWindow = nullptr;
    };

    ~AsyncRasterizer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            pendingJob_.reset();
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool TryConsume(const FrameKey& key, std::vector<std::uint32_t>& outPixels) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!readyValid_ || !(readyKey_ == key)) return false;
        outPixels = readyPixels_;
        return true;
    }

    void Submit(Job job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        if (readyValid_ && readyKey_ == job.key) return;
        if (pendingJob_ && pendingJob_->key == job.key) return;
        if (active_ && activeKey_ == job.key) return;
        pendingJob_ = std::move(job);
        if (!worker_.joinable()) worker_ = std::thread([this] { WorkerLoop(); });
        cv_.notify_one();
    }

private:
    void WorkerLoop() {
        HDC memDc = nullptr;
        HBITMAP dib = nullptr;
        HGDIOBJ oldBmp = nullptr;
        void* dibBits = nullptr;
        int dibW = 0, dibH = 0;
        HFONT workerFont = nullptr, workerSmallFont = nullptr;
        LOGFONTW cachedFontLog{}, cachedSmallFontLog{};
        bool hasCachedFont = false, hasCachedSmallFont = false;
        std::vector<std::uint32_t> scratchPixels;

        auto ensureDib = [&](int w, int h) -> bool {
            if (memDc && dib && dibW == w && dibH == h) return true;
            if (oldBmp && memDc) SelectObject(memDc, oldBmp);
            if (dib) DeleteObject(dib);
            dib = nullptr; oldBmp = nullptr; dibBits = nullptr; dibW = 0; dibH = 0;
            if (!memDc) memDc = CreateCompatibleDC(nullptr);
            if (!memDc || w <= 0 || h <= 0) return false;
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
            if (!dib || !dibBits) {
                if (dib) DeleteObject(dib);
                dib = nullptr;
                return false;
            }
            oldBmp = SelectObject(memDc, dib);
            dibW = w;
            dibH = h;
            return true;
        };

        auto ensureFont = [](const LOGFONTW& lf, bool valid, HFONT& handle, LOGFONTW& cached, bool& hasCached) -> HFONT {
            if (!valid) return nullptr;
            if (hasCached && handle && std::memcmp(&lf, &cached, sizeof(LOGFONTW)) == 0) return handle;
            if (handle) DeleteObject(handle);
            handle = CreateFontIndirectW(&lf);
            cached = lf;
            hasCached = handle != nullptr;
            return handle;
        };

        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || pendingJob_.has_value(); });
                if (stop_) break;
                job = std::move(*pendingJob_);
                pendingJob_.reset();
                active_ = true;
                activeKey_ = job.key;
            }

            bool rendered = false;
            if (ensureDib(job.params.width, job.params.height)) {
                job.params.font = ensureFont(job.fontLog, job.hasFontLog, workerFont, cachedFontLog, hasCachedFont);
                job.params.smallFont = ensureFont(job.smallFontLog, job.hasSmallFontLog, workerSmallFont, cachedSmallFontLog, hasCachedSmallFont);
                RECT client{0, 0, job.params.width, job.params.height};
                std::uint64_t gdiCalls = 0;
                const size_t base = job.params.topLine;
                RenderTerminalSurface(memDc, client, job.params,
                    [&](size_t lineIndex) -> const TerminalLine& {
                        return job.visibleLines[lineIndex - base];
                    }, gdiCalls);
                GdiFlush();
                const size_t pixelCount = static_cast<size_t>(dibW) * static_cast<size_t>(dibH);
                scratchPixels.resize(pixelCount);
                std::memcpy(scratchPixels.data(), dibBits, pixelCount * sizeof(std::uint32_t));
                rendered = true;
            }

            HWND notify = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_ = false;
                if (rendered && !stop_) {
                    readyKey_ = std::move(job.key);
                    readyPixels_.swap(scratchPixels);
                    readyValid_ = true;
                    notify = job.notifyWindow;
                }
            }
            if (notify && IsWindow(notify)) InvalidateRect(notify, nullptr, FALSE);
        }

        if (workerFont) DeleteObject(workerFont);
        if (workerSmallFont) DeleteObject(workerSmallFont);
        if (oldBmp && memDc) SelectObject(memDc, oldBmp);
        if (dib) DeleteObject(dib);
        if (memDc) DeleteDC(memDc);
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;
    bool active_ = false;
    FrameKey activeKey_;
    std::optional<Job> pendingJob_;
    bool readyValid_ = false;
    FrameKey readyKey_;
    std::vector<std::uint32_t> readyPixels_;
};

TerminalViewport::TerminalViewport() = default;
TerminalViewport::~TerminalViewport() = default;

void TerminalViewport::Paint(HDC dc, HWND terminal_, State* activeSession_, const Options& options) {
    const auto paintStarted = std::chrono::steady_clock::now();
    struct RecordPaint {
        PerformanceMetrics* metrics;
        std::chrono::steady_clock::time_point started;
        ~RecordPaint() {
            if (metrics) metrics->Record(PerformanceMetrics::Timing::Paint,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count()));
        }
    } recordPaint{options.performance, paintStarted};
    RECT client{};
    GetClientRect(terminal_, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    const auto theme = UiTheme(options.dark);
    const auto colors = theme;
    HDC buffer = paintBuffer_.Get(dc, client.right, client.bottom, Ui::Dpi);
    if (!buffer) buffer = dc;
    const auto& input = Input();
    const auto revision = activeSession_ ? activeSession_->terminal.Revision() : 0;
    const std::wstring draft = activeSession_ && options.mode == 4 ? activeSession_->cmdEditor.Text() : L"";
    const size_t draftCursor = activeSession_ && options.mode == 4 ? activeSession_->cmdEditor.Cursor() : 0;
    const bool focused = GetFocus() == terminal_;
    const bool reusable = buffer != dc && cache_.valid && cache_.state == activeSession_ &&
        cache_.revision == revision && cache_.bufferGeneration == paintBuffer_.Generation() &&
        cache_.width == client.right && cache_.height == client.bottom &&
        cache_.scroll == (activeSession_ ? activeSession_->terminalScrollOffset : 0) &&
        cache_.horizontal == (activeSession_ ? activeSession_->terminalHorizontalOffset : 0) &&
        cache_.anchorLine == input.anchorLine && cache_.focusLine == input.focusLine &&
        cache_.anchorColumn == input.anchorColumn && cache_.focusColumn == input.focusColumn &&
        cache_.selection == input.selectionActive &&
        cache_.newOutput == (activeSession_ && activeSession_->terminalHasNewOutput) &&
        cache_.focused == focused && cache_.dark == options.dark && cache_.timestamps == options.timestamps &&
        cache_.liveMonitor == options.liveMonitor && cache_.mode == options.mode &&
        cache_.font == options.font && cache_.smallFont == options.smallFont &&
        cache_.draft == draft && cache_.draftCursor == draftCursor &&
        cache_.emptyHint == options.emptyHint;
    if (reusable) {
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        if (options.measure)
            SetPropW(terminal_, L"SerialCtl.GdiCalls", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(1)));
        return;
    }
    const int savedBuffer = SaveDC(buffer);
    std::uint64_t gdiCalls = 1;

    auto updateCache = [&] {
        cache_.state = activeSession_;
        cache_.revision = revision;
        cache_.bufferGeneration = paintBuffer_.Generation();
        cache_.width = client.right; cache_.height = client.bottom;
        cache_.scroll = activeSession_ ? activeSession_->terminalScrollOffset : 0;
        cache_.horizontal = activeSession_ ? activeSession_->terminalHorizontalOffset : 0;
        cache_.anchorLine = input.anchorLine; cache_.focusLine = input.focusLine;
        cache_.anchorColumn = input.anchorColumn; cache_.focusColumn = input.focusColumn;
        cache_.selection = input.selectionActive;
        cache_.newOutput = activeSession_ && activeSession_->terminalHasNewOutput;
        cache_.focused = focused; cache_.dark = options.dark; cache_.timestamps = options.timestamps;
        cache_.liveMonitor = options.liveMonitor; cache_.mode = options.mode;
        cache_.font = options.font; cache_.smallFont = options.smallFont;
        cache_.draft = draft; cache_.draftCursor = draftCursor;
        cache_.emptyHint.assign(options.emptyHint);
        cache_.valid = buffer != dc;
    };

    if (activeSession_) {
        const TerminalModel& model = activeSession_->terminal;
        const TerminalModel* preview = nullptr;
        if (options.mode == 4 && !activeSession_->cmdEditor.Text().empty() &&
            activeSession_->terminalScrollOffset == 0)
            preview = &controller_.Preview(model, activeSession_->cmdEditor.Text(), activeSession_->cmdEditor.Cursor());
        else controller_.Clear();
        auto displayLine = [&](size_t i) -> const TerminalLine & {
            return preview && !model.LineWrapView()      ? preview->DisplayLine(i)
                   : preview && i >= model.HistorySize() ? preview->DisplayLine(i - model.HistorySize())
                                                         : model.DisplayLine(i);
        };
        const bool showTimestamps = options.timestamps && !options.liveMonitor && !model.AlternateScreen();
        const int padding = Ui::TerminalPadding;
        const int timestampWidth = showTimestamps ? Ui::TerminalTimestampWidth : 0;
        const int textLeft = padding + timestampWidth;
        const size_t totalLines =
            (preview ? (model.LineWrapView() ? model.HistorySize() : 0) + preview->DisplayLineCount()
                     : model.DisplayLineCount());
        const size_t totalColumns = preview ? preview->MaximumDisplayColumns() : model.MaximumDisplayColumns();
        const int maximumHorizontalOffset = std::max(0,
            static_cast<int>(totalColumns) - Metrics().columns);
        activeSession_->terminalHorizontalOffset = std::max(0,
            std::min(activeSession_->terminalHorizontalOffset, maximumHorizontalOffset));
        const int horizontalOffset = activeSession_->terminalHorizontalOffset;
        const int maximumOffset = std::max(0,
            static_cast<int>(totalLines) - Metrics().rows);
        activeSession_->terminalScrollOffset = std::max(0,
            std::min(activeSession_->terminalScrollOffset, maximumOffset));
        const size_t topLine = totalLines > static_cast<size_t>(Metrics().rows +
            activeSession_->terminalScrollOffset)
            ? totalLines - static_cast<size_t>(Metrics().rows +
                activeSession_->terminalScrollOffset)
            : 0;
        if (options.measure) {
            SetPropW(terminal_, L"SerialCtl.CellWidth", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(Metrics().cellWidth)));
            SetPropW(terminal_, L"SerialCtl.LineHeight", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(Metrics().lineHeight)));
            SetPropW(terminal_, L"SerialCtl.TextLeft", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(textLeft)));
            SetPropW(terminal_, L"SerialCtl.TextTop", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(padding)));
            SetPropW(terminal_, L"SerialCtl.VisibleColumns", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(Metrics().columns)));
            SetPropW(terminal_, L"SerialCtl.VisibleRows", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(Metrics().rows)));
            SetPropW(terminal_, L"SerialCtl.CmdDraftLength", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(activeSession_->cmdEditor.Text().size()) + 1));
            const auto anchor = model.CaptureAnchor(topLine);
            SetPropW(terminal_, L"SerialCtl.ScrollOffset",
                     reinterpret_cast<HANDLE>(static_cast<INT_PTR>(activeSession_->terminalScrollOffset) + 1));
            SetPropW(terminal_, L"SerialCtl.ReadAnchor", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(anchor.id)));
            SetPropW(terminal_, L"SerialCtl.ReadOffset",
                     reinterpret_cast<HANDLE>(static_cast<INT_PTR>(anchor.offset) + 1));
            SetPropW(terminal_, L"SerialCtl.NewOutput",
                     reinterpret_cast<HANDLE>(static_cast<INT_PTR>(activeSession_->terminalHasNewOutput) + 1));
            SetPropW(terminal_, L"SerialCtl.CmdInteractionBlocked",
                     reinterpret_cast<HANDLE>(static_cast<INT_PTR>(activeSession_->cmdInteractionWarning) + 1));
        }

        size_t selectionStartLine = Input().anchorLine;
        size_t selectionEndLine = Input().focusLine;
        int selectionStartColumn = Input().anchorColumn;
        int selectionEndColumn = Input().focusColumn;
        if (selectionStartLine > selectionEndLine ||
            (selectionStartLine == selectionEndLine && selectionStartColumn > selectionEndColumn)) {
            std::swap(selectionStartLine, selectionEndLine);
            std::swap(selectionStartColumn, selectionEndColumn);
        }

        const TerminalScrollMetrics scroll = GetTerminalScrollMetrics(terminal_,
            totalLines, Metrics().rows, activeSession_->terminalScrollOffset);
        const TerminalHorizontalScrollMetrics horizontalScroll =
            GetTerminalHorizontalScrollMetrics(terminal_, textLeft, totalColumns,
                Metrics().columns, activeSession_->terminalHorizontalOffset);

        RenderFrameParams params;
        params.width = client.right;
        params.height = client.bottom;
        params.padding = padding;
        params.textLeft = textLeft;
        params.cellWidth = Metrics().cellWidth;
        params.lineHeight = Metrics().lineHeight;
        params.columns = Metrics().columns;
        params.rows = Metrics().rows;
        params.horizontalOffset = horizontalOffset;
        params.scrollOffset = activeSession_->terminalScrollOffset;
        params.topLine = topLine;
        params.totalLines = totalLines;
        params.selectionStartLine = selectionStartLine;
        params.selectionEndLine = selectionEndLine;
        params.selectionStartColumn = selectionStartColumn;
        params.selectionEndColumn = selectionEndColumn;
        params.dark = options.dark;
        params.showTimestamps = showTimestamps;
        params.selectionActive = input.selectionActive;
        params.drawCursor = focused && model.CursorVisible() && activeSession_->terminalScrollOffset == 0;
        params.cursorLine = preview
            ? (model.LineWrapView() ? model.HistorySize() : 0) + preview->CursorDisplayLine()
            : model.CursorDisplayLine();
        params.cursorColumn = (preview ? preview->CursorColumn() : model.CursorColumn()) - horizontalOffset;
        params.showNewOutput = activeSession_->terminalHasNewOutput && activeSession_->terminalScrollOffset > 0;
        params.vScrollVisible = scroll.visible;
        params.hScrollVisible = horizontalScroll.visible;
        params.liveMonitor = options.liveMonitor;
        params.vThumb = scroll.thumb;
        params.hThumb = horizontalScroll.thumb;
        params.font = options.font;
        params.smallFont = options.smallFont;
        params.radius3 = Ui::Scale(3);
        params.radiusCard = Ui::Radius;
        params.outerRadius = Ui::Scale(8);

        if (!options.measure && buffer != dc && !registerPreview_) {
            FrameKey key{activeSession_, revision, client.right, client.bottom,
                activeSession_->terminalScrollOffset, activeSession_->terminalHorizontalOffset,
                input.anchorLine, input.focusLine, input.anchorColumn, input.focusColumn,
                input.selectionActive, activeSession_->terminalHasNewOutput, focused,
                options.dark, options.timestamps, options.liveMonitor, options.mode,
                options.font, options.smallFont, draft, draftCursor};
            std::vector<std::uint32_t> readyPixels;
            if (rasterizer_ && rasterizer_->TryConsume(key, readyPixels) &&
                readyPixels.size() == static_cast<size_t>(client.right) * static_cast<size_t>(client.bottom)) {
                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = client.right;
                bmi.bmiHeader.biHeight = -client.bottom;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                SetDIBitsToDevice(buffer, 0, 0, client.right, client.bottom, 0, 0, 0, client.bottom,
                                  readyPixels.data(), &bmi, DIB_RGB_COLORS);
                BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
                updateCache();
                RestoreDC(buffer, savedBuffer);
                return;
            }
            if (cache_.valid && cache_.state == activeSession_ &&
                cache_.width == client.right && cache_.height == client.bottom &&
                cache_.bufferGeneration == paintBuffer_.Generation() &&
                cache_.dark == options.dark) {
                if (!rasterizer_) rasterizer_ = std::make_unique<AsyncRasterizer>();
                AsyncRasterizer::Job job;
                job.key = std::move(key);
                job.params = params;
                job.params.font = nullptr;
                job.params.smallFont = nullptr;
                job.hasFontLog = options.font && GetObjectW(options.font, sizeof(LOGFONTW), &job.fontLog) > 0;
                job.hasSmallFontLog = options.smallFont && GetObjectW(options.smallFont, sizeof(LOGFONTW), &job.smallFontLog) > 0;
                job.notifyWindow = terminal_;
                const size_t endLine = std::min(totalLines, topLine + static_cast<size_t>(Metrics().rows));
                if (endLine > topLine) {
                    job.visibleLines.reserve(endLine - topLine);
                    for (size_t i = topLine; i < endLine; ++i)
                        job.visibleLines.push_back(displayLine(i));
                }
                rasterizer_->Submit(std::move(job));
                BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
                RestoreDC(buffer, savedBuffer);
                return;
            }
        }

        RenderTerminalSurface(buffer, client, params, displayLine, gdiCalls);

        registerCardBounds_ = {};
        registerBitBounds_.fill(RECT{});
        if (registerPreview_ && focused && input.selectionActive && !input.selecting) {
            if (SelectionText(*activeSession_, false) != registerPreview_->selection) {
                registerPreview_.reset();
            } else if (selectionStartLine < topLine + static_cast<size_t>(Metrics().rows) && selectionEndLine >= topLine) {
                const int firstRow = static_cast<int>(std::max(selectionStartLine, topLine) - topLine);
                const int lastRow = static_cast<int>(std::min(selectionEndLine - topLine,
                                                             static_cast<size_t>(Metrics().rows - 1)));
                const bool singleLine = selectionStartLine == selectionEndLine;
                const int textRight = client.right - Ui::OverlayScrollLaneWidth;
                const RECT selection{
                    singleLine ? std::max(textLeft, textLeft + (selectionStartColumn - horizontalOffset) * Metrics().cellWidth)
                               : textLeft,
                    padding + firstRow * Metrics().lineHeight,
                    singleLine ? std::min(textRight, textLeft + (selectionEndColumn + 1 - horizontalOffset) * Metrics().cellWidth)
                               : textRight,
                    padding + (lastRow + 1) * Metrics().lineHeight};
                if (selection.left < selection.right)
                    DrawRegisterPreview(buffer, client, selection, registerPreview_->value, options,
                                        &registerCardBounds_, &registerBitBounds_);
            }
        }
    } else {
        RenderFrameParams emptyParams;
        emptyParams.width = client.right;
        emptyParams.height = client.bottom;
        emptyParams.dark = options.dark;
        emptyParams.liveMonitor = options.liveMonitor;
        emptyParams.outerRadius = Ui::Scale(8);
        RenderTerminalSurface(buffer, client, emptyParams,
            [](size_t) -> const TerminalLine& { static const TerminalLine kEmpty{}; return kEmpty; }, gdiCalls);
        if (!options.emptyHint.empty()) {
            const int inset = Ui::Scale(16);
            RECT hintRect{client.left + inset, client.top + inset, client.right - inset, client.bottom - inset};
            if (hintRect.right > hintRect.left && hintRect.bottom > hintRect.top) {
                HFONT hintFont = options.smallFont ? options.smallFont : options.font;
                HGDIOBJ oldFont = hintFont ? SelectObject(buffer, hintFont) : nullptr;
                const int oldMode = SetBkMode(buffer, TRANSPARENT);
                const COLORREF oldColor = SetTextColor(buffer, colors.muted);
                RECT measured = hintRect;
                const int h = DrawTextW(buffer, options.emptyHint.data(), static_cast<int>(options.emptyHint.size()),
                                        &measured, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
                if (h > 0 && h < (hintRect.bottom - hintRect.top)) {
                    hintRect.top += ((hintRect.bottom - hintRect.top) - h) / 2;
                    hintRect.bottom = hintRect.top + h;
                }
                DrawTextW(buffer, options.emptyHint.data(), static_cast<int>(options.emptyHint.size()),
                          &hintRect, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
                ++gdiCalls;
                SetTextColor(buffer, oldColor);
                SetBkMode(buffer, oldMode);
                if (oldFont)
                    SelectObject(buffer, oldFont);
            }
        }
    }

    BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    ++gdiCalls;
    if (options.measure)
        SetPropW(terminal_, L"SerialCtl.GdiCalls", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(gdiCalls)));
    if (options.measure && activeSession_ && activeSession_->pendingProbeArrival) {
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        auto microseconds =
            static_cast<INT_PTR>((now.QuadPart - activeSession_->pendingProbeArrival) * 1000000 / frequency.QuadPart);
        SetPropW(terminal_, L"SerialCtl.PaintUsec", reinterpret_cast<HANDLE>(std::max<INT_PTR>(1, microseconds)));
        SetPropW(terminal_, L"SerialCtl.PaintSample", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(++paintSample_)));
        SetPropW(terminal_, L"SerialCtl.PaintProbe", reinterpret_cast<HANDLE>(activeSession_->paintProbe));
        activeSession_->pendingProbeArrival = 0;
    }
    updateCache();
    RestoreDC(buffer, savedBuffer);
}
void TerminalViewport::Measure(HWND window, HFONT font, bool timestamps) {
    RECT client{}; GetClientRect(window, &client);
    HDC dc = GetDC(window);
    auto previous = SelectObject(dc, font);
    TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    SIZE characterSize{}; GetTextExtentPoint32W(dc, L"M", 1, &characterSize);
    SelectObject(dc, previous); ReleaseDC(window, dc);
    auto& grid = Metrics();
    grid.cellWidth = std::max(Ui::Scale(7), static_cast<int>(characterSize.cx));
    grid.lineHeight = std::max(Ui::Scale(15), static_cast<int>(metrics.tmHeight) + Ui::Scale(2));
    const int timestampWidth = timestamps ? Ui::TerminalTimestampWidth : 0;
    const int width = std::max(1, static_cast<int>(client.right) - Ui::TerminalPadding * 2 - timestampWidth - Ui::OverlayScrollLaneWidth);
    const int height = std::max(1, static_cast<int>(client.bottom) - Ui::TerminalPadding * 2);
    grid.columns = std::max(20, width / grid.cellWidth); grid.rows = std::max(2, height / grid.lineHeight);
}
bool TerminalViewport::PointToCell(HWND window, const State& state, POINT point, const Options& options,
                                   size_t& line, int& column) const {
    RECT client{}; GetClientRect(window, &client);
    const bool timestamps = options.timestamps && !options.liveMonitor && !state.terminal.AlternateScreen();
    return controller_.PointToCell(state, point.x, point.y,
        Ui::TerminalPadding + (timestamps ? Ui::TerminalTimestampWidth : 0), Ui::TerminalPadding,
        client.right - Ui::OverlayScrollLaneWidth, line, column);
}

TerminalViewport::PointerResult TerminalViewport::Pointer(HWND window, UINT message, LPARAM lParam,
                                                          State* session, const Options& options) {
    if (message != WM_LBUTTONDOWN && message != WM_LBUTTONDBLCLK && message != WM_MOUSEMOVE &&
        message != WM_LBUTTONUP && message != WM_CAPTURECHANGED) return PointerResult::Unhandled;
    const auto scroll = [&](int lines) { if (session) { Scroll(*session, lines); InvalidateRect(window, nullptr, FALSE); } };
    const auto scrollHorizontal = [&](int columns) { if (session) { ScrollHorizontal(*session, columns); InvalidateRect(window, nullptr, FALSE); } };
    const auto toBottom = [&] { if (session) { ToBottom(*session); InvalidateRect(window, nullptr, FALSE); } };
    const auto pointToCell = [&](POINT point, size_t& line, int& column) {
        return session && PointToCell(window, *session, point, options, line, column);
    };
    const auto select = [&](size_t line, int column, bool extend) { Select(line, column, extend); InvalidateRect(window, nullptr, FALSE); };
    const auto clearSelection = [&] { ClearSelection(); InvalidateRect(window, nullptr, FALSE); };
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        const POINT click{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (registerPreview_ && PointInRect(registerCardBounds_, click)) {
            for (unsigned bit = 0; bit < 32; ++bit) {
                if (PointInRect(registerBitBounds_[bit], click)) {
                    registerPreview_->value = FlipRegisterBit(registerPreview_->value, bit);
                    cache_.valid = false;
                    InvalidateRect(window, &registerCardBounds_, FALSE);
                    break;
                }
            }
            SetFocus(window);
            return PointerResult::Handled; // Inside the card: don't start or replace text selection.
        }
    }
    if (message == WM_LBUTTONDOWN) {
        DismissRegisterPreview(window);
        SetFocus(window);
        if (!session) return PointerResult::Handled;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const TerminalModel& model = session->terminal;
        const int timestampWidth =
            options.timestamps && !options.liveMonitor && !model.AlternateScreen() ? Ui::TerminalTimestampWidth : 0;
        const TerminalHorizontalScrollMetrics horizontalMetrics =
            GetTerminalHorizontalScrollMetrics(window, Ui::TerminalPadding + timestampWidth,
                model.MaximumDisplayColumns(), Metrics().columns,
                session->terminalHorizontalOffset);
        if (horizontalMetrics.visible && point.y >= horizontalMetrics.track.top &&
            point.x >= horizontalMetrics.track.left && point.x < horizontalMetrics.track.right) {
            if (PointInRect(horizontalMetrics.thumb, point)) {
                Input().horizontalScrollbarDragging = true;
                Input().horizontalScrollbarThumbOffset = point.x - horizontalMetrics.thumb.left;
                SetCapture(window);
            } else {
                const int direction = point.x < horizontalMetrics.thumb.left ? -1 : 1;
                scrollHorizontal(
                    direction * std::max(1, Metrics().columns - 1));
            }
            return PointerResult::Handled;
        }
        const TerminalScrollMetrics metrics = GetTerminalScrollMetrics(window,
            session->terminal.DisplayLineCount(), Metrics().rows,
            session->terminalScrollOffset);
        if (metrics.visible && point.x >= metrics.track.left) {
            if (PointInRect(metrics.thumb, point)) {
                Input().scrollbarDragging = true;
                Input().scrollbarThumbOffset = point.y - metrics.thumb.top;
                SetCapture(window);
            } else {
                const int direction = point.y < metrics.thumb.top ? 1 : -1;
                scroll(direction * std::max(1, Metrics().rows - 1));
            }
            return PointerResult::Handled;
        }
        RECT client{};
        GetClientRect(window, &client);
        if (session->terminalHasNewOutput && session->terminalScrollOffset > 0 &&
            PointInRect(TerminalNewOutputRect(client, horizontalMetrics.visible), point)) {
            toBottom();
            return PointerResult::Handled;
        }
        size_t line = 0;
        int column = 0;
        if (pointToCell(point, line, column)) {
            select(line, column, false);
            Input().selecting = true;
            SetCapture(window);
        }
        return PointerResult::Handled;
    }
    if (message == WM_LBUTTONDBLCLK && session) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        size_t lineIndex = 0;
        int column = 0;
        if (pointToCell(point, lineIndex, column)) {
            const TerminalLine& line = session->terminal.DisplayLine(lineIndex);
            const int columns = static_cast<int>(line.cells.size());
            if (columns <= 0) return PointerResult::Handled;
            column = std::max(0, std::min(columns - 1, column));
            const auto wordCharacter = [](wchar_t character) {
                return character && (iswalnum(character) || wcschr(L"_./~:+-%@", character) != nullptr);
            };
            int first = column;
            int last = column;
            while (first > 0 && wordCharacter(line.cells[static_cast<size_t>(first - 1)].character)) --first;
            while (last + 1 < columns && wordCharacter(line.cells[static_cast<size_t>(last + 1)].character)) ++last;
            Input().anchorLine = lineIndex;
            Input().anchorColumn = first;
            Input().focusLine = lineIndex;
            Input().focusColumn = last;
            Input().selectionActive = true;
            Input().selecting = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            return PointerResult::CopySelection;
        }
        return PointerResult::Handled;
    }
    if (message == WM_MOUSEMOVE && session) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (Input().horizontalScrollbarDragging) {
            const TerminalModel& model = session->terminal;
            const int timestampWidth = options.timestamps && !options.liveMonitor && !model.AlternateScreen()
                                           ? Ui::TerminalTimestampWidth
                                           : 0;
            const TerminalHorizontalScrollMetrics metrics =
                GetTerminalHorizontalScrollMetrics(window, Ui::TerminalPadding + timestampWidth,
                    model.MaximumDisplayColumns(), Metrics().columns,
                    session->terminalHorizontalOffset);
            const int thumbWidth = static_cast<int>(metrics.thumb.right - metrics.thumb.left);
            const int travel = std::max(1,
                static_cast<int>(metrics.track.right - metrics.track.left) - thumbWidth);
            const int desiredLeft = std::max(static_cast<int>(metrics.track.left),
                std::min(static_cast<int>(metrics.track.right) - thumbWidth,
                    static_cast<int>(point.x) - Input().horizontalScrollbarThumbOffset));
            session->terminalHorizontalOffset = std::max(0,
                std::min(metrics.maximumOffset,
                    (desiredLeft - static_cast<int>(metrics.track.left)) *
                        metrics.maximumOffset / travel));
            InvalidateRect(window, nullptr, FALSE);
            return PointerResult::Handled;
        }
        if (Input().scrollbarDragging) {
            const TerminalScrollMetrics metrics = GetTerminalScrollMetrics(window,
                session->terminal.DisplayLineCount(), Metrics().rows,
                session->terminalScrollOffset);
            const int thumbHeight = static_cast<int>(metrics.thumb.bottom - metrics.thumb.top);
            const int travel = std::max(1,
                static_cast<int>(metrics.track.bottom - metrics.track.top) - thumbHeight);
            const int desiredTop = std::max(static_cast<int>(metrics.track.top),
                std::min(static_cast<int>(metrics.track.bottom) - thumbHeight,
                    static_cast<int>(point.y) - Input().scrollbarThumbOffset));
            session->terminalScrollOffset = std::max(0,
                std::min(metrics.maximumOffset,
                    (static_cast<int>(metrics.track.top) + travel - desiredTop) *
                        metrics.maximumOffset / travel));
            session->terminalHasNewOutput =
                session->terminalScrollOffset > 0;
            InvalidateRect(window, nullptr, FALSE);
            return PointerResult::Handled;
        }
        if (Input().selecting) {
            size_t line = 0;
            int column = 0;
            if (pointToCell(point, line, column))
                select(line, column, true);
            return PointerResult::Handled;
        }
    }
    if (message == WM_LBUTTONUP) {
        if (Input().selecting) {
            size_t line = 0;
            int column = 0;
            if (pointToCell({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, line, column))
                select(line, column, true);
        }
        const bool completedSelection = Input().selecting && (session && Input().selectionActive);
        Input().selecting = false;
        Input().scrollbarDragging = false;
        Input().scrollbarThumbOffset = 0;
        Input().horizontalScrollbarDragging = false;
        Input().horizontalScrollbarThumbOffset = 0;
        if (GetCapture() == window) ReleaseCapture();
        if (completedSelection) return PointerResult::CopySelection;
        return PointerResult::Handled;
    }
    if (message == WM_CAPTURECHANGED) {
        Input().selecting = false;
        Input().scrollbarDragging = false;
        Input().scrollbarThumbOffset = 0;
        Input().horizontalScrollbarDragging = false;
        Input().horizontalScrollbarThumbOffset = 0;
    }
    return PointerResult::Unhandled;
}

} // namespace serialctl
