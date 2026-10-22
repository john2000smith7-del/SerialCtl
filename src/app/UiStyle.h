#pragma once
#include <algorithm>
#include <climits>
#include <map>
#include <new>
#include <string>
#include <vector>
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
// Windows SDK GDI+ declarations use COM/property interfaces.
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
namespace serialctl {
struct UiColors {
    COLORREF surface, raised, field, text, muted, border, accent, danger, window, accentSoft;
};
inline UiColors UiTheme(bool dark) {
    return dark ? UiColors{RGB(36, 36, 38),    RGB(44, 44, 46), RGB(20, 20, 22),   RGB(245, 245, 247),
                           RGB(152, 152, 157), RGB(58, 58, 60), RGB(10, 132, 255), RGB(255, 69, 58),
                           RGB(28, 28, 30), RGB(31, 55, 80)}
                : UiColors{RGB(255, 255, 255), RGB(242, 242, 247), RGB(255, 255, 255), RGB(29, 29, 31),
                           RGB(110, 110, 115), RGB(210, 210, 215), RGB(0, 122, 255),   RGB(255, 59, 48),
                           RGB(245, 245, 247), RGB(229, 241, 255)};
}
inline Gdiplus::Color UiColor(COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}
// Build a complete owner-drawn item off screen, then publish it with one blit.
// Keep the caller's logical coordinates (list rows need not start at 0,0).
// The destination DC retains its native update/visibility clip on the final blit.
class UiBufferedPaint {
  public:
    UiBufferedPaint(HDC target, const RECT &bounds) : target_(target), bounds_(bounds) {
        if (!target_)
            return;
        savedTarget_ = SaveDC(target_);
        const auto width = static_cast<long long>(bounds.right) - bounds.left;
        const auto height = static_cast<long long>(bounds.bottom) - bounds.top;
        // These painters use pixel coordinates. Preserve the original direct
        // drawing path for an unusual mapped/transformed DC or allocation failure.
        if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX ||
            GetMapMode(target_) != MM_TEXT || GetGraphicsMode(target_) != GM_COMPATIBLE ||
            GetLayout(target_) != 0)
            return;
        width_ = static_cast<int>(width);
        height_ = static_cast<int>(height);
        buffer_ = CreateCompatibleDC(target_);
        if (!buffer_)
            return;
        bitmap_ = CreateCompatibleBitmap(target_, width_, height_);
        if (!bitmap_)
            return;
        previousBitmap_ = SelectObject(buffer_, bitmap_);
        if (!previousBitmap_ || previousBitmap_ == HGDI_ERROR)
            return;
        if (!SetWindowOrgEx(buffer_, bounds.left, bounds.top, nullptr))
            return;
        // Preserve untouched pixels for callbacks which only draw a focus state.
        if (!BitBlt(buffer_, bounds.left, bounds.top, width_, height_,
                    target_, bounds.left, bounds.top, SRCCOPY))
            return;
        SelectObject(buffer_, GetCurrentObject(target_, OBJ_FONT));
        SelectObject(buffer_, GetCurrentObject(target_, OBJ_PEN));
        SelectObject(buffer_, GetCurrentObject(target_, OBJ_BRUSH));
        SetTextColor(buffer_, GetTextColor(target_));
        SetBkColor(buffer_, GetBkColor(target_));
        SetBkMode(buffer_, GetBkMode(target_));
        SetTextAlign(buffer_, GetTextAlign(target_));
        SetPolyFillMode(buffer_, GetPolyFillMode(target_));
        SetROP2(buffer_, GetROP2(target_));
        SetStretchBltMode(buffer_, GetStretchBltMode(target_));
        ready_ = true;
    }
    UiBufferedPaint(const UiBufferedPaint &) = delete;
    UiBufferedPaint &operator=(const UiBufferedPaint &) = delete;
    HDC Dc() const {
        return ready_ ? buffer_ : target_;
    }
    ~UiBufferedPaint() {
        if (ready_)
            BitBlt(target_, bounds_.left, bounds_.top, width_, height_,
                   buffer_, bounds_.left, bounds_.top, SRCCOPY);
        if (previousBitmap_ && previousBitmap_ != HGDI_ERROR)
            SelectObject(buffer_, previousBitmap_);
        if (bitmap_)
            DeleteObject(bitmap_);
        if (buffer_)
            DeleteDC(buffer_);
        if (savedTarget_)
            RestoreDC(target_, savedTarget_);
    }

  private:
    HDC target_ = nullptr, buffer_ = nullptr;
    RECT bounds_{};
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    int savedTarget_ = 0, width_ = 0, height_ = 0;
    bool ready_ = false;
};
inline void UiBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    const float width = static_cast<float>(rect.right - rect.left - 1);
    const float height = static_cast<float>(rect.bottom - rect.top - 1);
    if (!dc || width <= 0 || height <= 0)
        return;
    // App.cpp owns GDI+ startup/shutdown. The path's 1 px stroke is entirely
    // inside the supplied bounds; callers paint the surrounding surface first.
    const float x = rect.left + 0.5f, y = rect.top + 0.5f;
    const float diameter = std::min(static_cast<float>(std::max(0, radius) * 2), std::min(width, height));
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    if (diameter > 0) {
        path.AddArc(x, y, diameter, diameter, 180, 90);
        path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
        path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
        path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
    } else
        path.AddRectangle(Gdiplus::RectF(x, y, width, height));
    Gdiplus::SolidBrush brush(UiColor(fill));
    Gdiplus::Pen pen(UiColor(border), 1.0f);
    graphics.FillPath(&brush, &path);
    graphics.DrawPath(&pen, &path);
}
inline void UiPolyline(HDC dc, const POINT *points, int count, COLORREF color, float width = 1.0f) {
    if (!dc || !points || count < 2)
        return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(UiColor(color), width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::GraphicsPath path;
    for (int i = 1; i < count; ++i)
        path.AddLine(points[i - 1].x + 0.5f, points[i - 1].y + 0.5f, points[i].x + 0.5f, points[i].y + 0.5f);
    graphics.DrawPath(&pen, &path);
}
struct UiFieldStyle {
    bool *dark;
    HFONT font;
    int dpi;
};
// Keep native COMBOBOX/ComboLBox windows, selection notifications, keyboard
// handling and accessibility. Only painting and the scrollbar lane are shared.
struct UiComboPopup {
    HWND combo = nullptr;
    UiFieldStyle *style = nullptr;
    bool dragging = false;
    int dragOffset = 0;
    bool ownsCapture = false;
    int wheelRemainder = 0;
};
struct UiComboScroll {
    RECT lane{}, track{}, thumb{};
    int top = 0, page = 0, maximum = 0;
};
inline bool UiComboMetrics(HWND list, const UiFieldStyle &style, UiComboScroll &scroll) {
    RECT bounds{};
    GetClientRect(list, &bounds);
    const int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    const int row = static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
    if (count <= 0 || row <= 0 || bounds.bottom <= 0)
        return false;
    const int inset = std::max(1, MulDiv(4, style.dpi, 96));
    scroll.page = std::max(1, static_cast<int>(bounds.bottom) / row);
    scroll.maximum = std::max(0, count - scroll.page);
    if (!scroll.maximum)
        return false;
    scroll.top = std::max(0, std::min(scroll.maximum, static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0))));
    scroll.lane = {bounds.right - MulDiv(16, style.dpi, 96), 1, bounds.right - 1, bounds.bottom - 1};
    scroll.track = {bounds.right - MulDiv(8, style.dpi, 96), inset,
                    bounds.right - MulDiv(3, style.dpi, 96), bounds.bottom - inset};
    const int trackHeight = std::max(1L, scroll.track.bottom - scroll.track.top);
    const int thumbHeight = std::min(trackHeight, std::max(MulDiv(24, style.dpi, 96), trackHeight * scroll.page / count));
    const int y = scroll.track.top + (trackHeight - thumbHeight) * scroll.top / scroll.maximum;
    scroll.thumb = {scroll.track.left, y, scroll.track.right, y + thumbHeight};
    return true;
}
inline void UiComboRegion(HWND list, int dpi) {
    RECT bounds{};
    GetWindowRect(list, &bounds);
    const int radius = MulDiv(10, dpi, 96);
    // The integer window region sits OUTSIDE the painted border. Clipping the
    // antialiased curve to an identical integer region would restore jaggies.
    HRGN region = CreateRoundRectRgn(-2, -2, bounds.right - bounds.left + 2,
                                     bounds.bottom - bounds.top + 2, (radius + 2) * 2, (radius + 2) * 2);
    if (region && !SetWindowRgn(list, region, FALSE))
        DeleteObject(region);
}
inline void UiPaintComboRow(HDC dc, HWND combo, int index, RECT row, bool selected,
                           const UiFieldStyle &style, const UiComboScroll *scroll = nullptr) {
    const auto colors = UiTheme(*style.dark);
    auto d = [&](int value) { return MulDiv(value, style.dpi, 96); };
    HBRUSH background = CreateSolidBrush(colors.field);
    FillRect(dc, &row, background);
    DeleteObject(background);
    row.left += d(2);
    row.right = scroll ? scroll->lane.left : row.right - d(2);
    if (selected) {
        RECT highlight = row;
        InflateRect(&highlight, 0, -1);
        UiBox(dc, highlight, colors.accentSoft, colors.accentSoft, d(8));
    }
    const int length = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, index, 0));
    if (length < 0 || length > 65536)
        return;
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    row.left += d(6);
    row.right -= d(6);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, !IsWindowEnabled(combo) ? colors.muted : selected ? colors.accent : colors.text);
    DrawTextW(dc, text.data(), length, &row, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}
inline void UiPaintComboField(HWND combo, HDC target, const UiFieldStyle &style) {
    RECT bounds{};
    GetClientRect(combo, &bounds);
    if (!target || bounds.right <= 0 || bounds.bottom <= 0)
        return;
    UiBufferedPaint paint(target, bounds);
    HDC dc = paint.Dc();
    const int saved = SaveDC(dc);
    const auto colors = UiTheme(*style.dark);
    auto d = [&](int value) { return MulDiv(value, style.dpi, 96); };
    wchar_t parentClass[32]{};
    GetClassNameW(GetParent(combo), parentClass, 32);
    const COLORREF outsideColor = GetPropW(combo, L"SerialCtl.FieldSurface") || _wcsicmp(parentClass, L"#32770") == 0
                                      ? colors.surface
                                      : colors.field;
    HBRUSH outside = CreateSolidBrush(outsideColor);
    FillRect(dc, &bounds, outside);
    DeleteObject(outside);
    const bool enabled = IsWindowEnabled(combo) != FALSE;
    const bool focused = enabled && (GetFocus() == combo || SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0));
    const COLORREF border = focused                                               ? colors.accent
                            : enabled && GetPropW(combo, L"SerialCtl.FieldHover") ? colors.muted
                                                                                  : colors.border;
    UiBox(dc, bounds, colors.field, border, d(10));
    const int selected = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    std::wstring text;
    if (selected >= 0) {
        const int length = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, selected, 0));
        if (length >= 0 && length < 65536) {
            std::vector<wchar_t> buffer(length + 1);
            SendMessageW(combo, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(buffer.data()));
            text.assign(buffer.data());
        }
    }
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0));
    if (font || style.font)
        SelectObject(dc, font ? font : style.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? colors.text : colors.muted);
    RECT label = bounds;
    label.left += d(8);
    label.right -= d(28);
    DrawTextW(dc, text.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    const int x = bounds.right - d(14), y = bounds.bottom / 2;
    const POINT chevron[]{{x - d(4), y - d(2)}, {x, y + d(2)}, {x + d(4), y - d(2)}};
    UiPolyline(dc, chevron, 3, colors.muted, static_cast<float>(d(2)));
    RestoreDC(dc, saved);
}
inline void UiDrawComboItem(const DRAWITEMSTRUCT &source, const UiFieldStyle &style) {
    auto item = source;
    // Selection/focus changes can paint synchronously outside WM_PAINT. Native
    // code may already have filled the rectangular selection area. Use the same
    // field geometry on its supplied DC, preserving native clipping/printing.
    // UiFieldProc restores the full field after the native transition returns.
    if (item.itemState & ODS_COMBOBOXEDIT) {
        UiPaintComboField(item.hwndItem, item.hDC, style);
        return;
    }
    UiBufferedPaint paint(item.hDC, item.rcItem);
    item.hDC = paint.Dc();
    const int saved = SaveDC(item.hDC);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (font)
        SelectObject(item.hDC, font);
    COMBOBOXINFO info{sizeof(info)};
    UiComboScroll scroll;
    const bool scrolling = GetComboBoxInfo(item.hwndItem, &info) && UiComboMetrics(info.hwndList, style, scroll);
    UiPaintComboRow(item.hDC, item.hwndItem, static_cast<int>(item.itemID), item.rcItem,
                    (item.itemState & ODS_SELECTED) != 0, style, scrolling ? &scroll : nullptr);
    RestoreDC(item.hDC, saved);
}
inline void UiPaintComboPopup(HWND list, HDC target, const UiComboPopup &data) {
    RECT bounds{};
    GetClientRect(list, &bounds);
    if (!target || bounds.right <= 0 || bounds.bottom <= 0)
        return;
    auto colors = UiTheme(*data.style->dark);
    auto d = [&](int value) { return MulDiv(value, data.style->dpi, 96); };
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, bounds.right, bounds.bottom) : nullptr;
    HGDIOBJ previousBitmap = bitmap ? SelectObject(buffer, bitmap) : nullptr;
    HDC dc = bitmap ? buffer : target;
    const int saved = SaveDC(dc);
    HBRUSH background = CreateSolidBrush(colors.field);
    FillRect(dc, &bounds, background);
    DeleteObject(background);
    UiComboScroll scroll;
    const bool scrolling = UiComboMetrics(list, *data.style, scroll);
    const int count = static_cast<int>(SendMessageW(data.combo, CB_GETCOUNT, 0, 0));
    const int top = std::max(0, static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0)));
    const int selected = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(data.combo, WM_GETFONT, 0, 0));
    if (font)
        SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    for (int i = top; i < count; ++i) {
        RECT row{};
        if (SendMessageW(list, LB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&row)) == LB_ERR || row.top >= bounds.bottom)
            break;
        if (row.bottom <= 0)
            continue;
        UiPaintComboRow(dc, data.combo, i, row, i == selected, *data.style, scrolling ? &scroll : nullptr);
    }
    if (scrolling)
        UiBox(dc, scroll.thumb, colors.border, colors.border, d(3));
    // Draw only the outline here: filling UiBox would cover the native rows.
    {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x = 0.5f, y = 0.5f, width = bounds.right - 1.0f, height = bounds.bottom - 1.0f;
    const float diameter = std::min(static_cast<float>(d(20)), std::min(width, height));
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    Gdiplus::Pen pen(UiColor(colors.border), 1.0f);
    graphics.DrawPath(&pen, &path);
    }
    RestoreDC(dc, saved);
    if (bitmap) {
        BitBlt(target, 0, 0, bounds.right, bounds.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previousBitmap);
        DeleteObject(bitmap);
    }
    if (buffer)
        DeleteDC(buffer);
}
inline LRESULT CALLBACK UiComboPopupProc(HWND list, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR reference) {
    auto *data = reinterpret_cast<UiComboPopup *>(reference);
    if (message == WM_NCCALCSIZE || message == WM_NCPAINT)
        return 0;
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT || message == WM_PRINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? BeginPaint(list, &ps) : reinterpret_cast<HDC>(w);
        UiPaintComboPopup(list, dc, *data);
        if (message == WM_PAINT)
            EndPaint(list, &ps);
        return 0;
    }
    if (message == WM_STYLECHANGING && w == GWL_STYLE)
        reinterpret_cast<STYLESTRUCT *>(l)->styleNew &= ~(WS_BORDER | WS_VSCROLL | WS_HSCROLL);
    if (message == WM_WINDOWPOSCHANGING) {
        auto &position = *reinterpret_cast<WINDOWPOS *>(l);
        if (!(position.flags & SWP_NOSIZE)) {
            const int count = static_cast<int>(SendMessageW(data->combo, CB_GETCOUNT, 0, 0));
            const int row = static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
            MONITORINFO monitor{sizeof(monitor)};
            RECT combo{};
            GetWindowRect(data->combo, &combo);
            if (count > 0 && row > 0 && GetMonitorInfoW(MonitorFromWindow(data->combo, MONITOR_DEFAULTTONEAREST), &monitor)) {
                const int below = std::max(0L, monitor.rcWork.bottom - combo.bottom);
                const int above = std::max(0L, combo.top - monitor.rcWork.top);
                const int available = std::max(below, above);
                // Fixed built-in choices all fit; large dynamic lists use the
                // shared overlay scrollbar and never leave the work area.
                const int rows = std::max(1, std::min(count, std::max(1, (available - 2) / row)));
                position.cy = std::min(available, rows * row + 2);
                if (position.flags & SWP_NOMOVE) {
                    RECT current{};
                    GetWindowRect(list, &current);
                    position.x = current.left;
                }
                position.y = position.cy <= below ? combo.bottom : combo.top - position.cy;
                position.flags &= ~SWP_NOMOVE;
            }
        }
    }
    UiComboScroll scroll;
    if (message == WM_MOUSEWHEEL && UiComboMetrics(list, *data->style, scroll)) {
        data->wheelRemainder += GET_WHEEL_DELTA_WPARAM(w);
        const int notches = data->wheelRemainder / WHEEL_DELTA;
        data->wheelRemainder %= WHEEL_DELTA;
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const int rows = lines == WHEEL_PAGESCROLL ? scroll.page : static_cast<int>(std::min(lines, 100u));
        SendMessageW(list, LB_SETTOPINDEX,
                     std::max(0, std::min(scroll.maximum, scroll.top - rows * notches)), 0);
        InvalidateRect(list, nullptr, FALSE);
        UpdateWindow(list);
        return 0;
    }
    const POINT mouse{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) &&
        UiComboMetrics(list, *data->style, scroll) && PtInRect(&scroll.lane, mouse)) {
        data->dragging = true;
        const int y = GET_Y_LPARAM(l);
        data->dragOffset = y >= scroll.thumb.top && y < scroll.thumb.bottom
                               ? y - scroll.thumb.top
                               : (scroll.thumb.bottom - scroll.thumb.top) / 2;
        // Keep the native combo's capture: stealing it may dismiss the list.
        // UiFieldProc forwards an active drag if the combo owns capture.
        data->ownsCapture = GetCapture() == nullptr;
        if (data->ownsCapture)
            SetCapture(list);
        SendMessageW(list, WM_MOUSEMOVE, MK_LBUTTON, l);
        return 0;
    }
    if (message == WM_MOUSEMOVE && data->dragging) {
        if (UiComboMetrics(list, *data->style, scroll)) {
            const int travel = std::max(1L, scroll.track.bottom - scroll.track.top - (scroll.thumb.bottom - scroll.thumb.top));
            const int offset = std::max(0, std::min(travel, GET_Y_LPARAM(l) - data->dragOffset - static_cast<int>(scroll.track.top)));
            SendMessageW(list, LB_SETTOPINDEX, offset * scroll.maximum / travel, 0);
            InvalidateRect(list, nullptr, FALSE);
            UpdateWindow(list);
        }
        return 0;
    }
    if (message == WM_LBUTTONUP && data->dragging) {
        data->dragging = false;
        const bool release = data->ownsCapture && GetCapture() == list;
        data->ownsCapture = false;
        if (release)
            ReleaseCapture();
        return 0;
    }
    if (message == WM_CAPTURECHANGED) {
        data->dragging = false;
        data->ownsCapture = false;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(list, UiComboPopupProc, id);
        delete data;
        return DefSubclassProc(list, message, w, l);
    }
    const LRESULT result = DefSubclassProc(list, message, w, l);
    // A selection notification can close the owning dialog synchronously.
    // Never dereference its popup data after the native procedure destroyed it.
    DWORD_PTR current = 0;
    if (!GetWindowSubclass(list, UiComboPopupProc, id, &current) || current != reference)
        return result;
    if (message == WM_SIZE || message == WM_WINDOWPOSCHANGED)
        UiComboRegion(list, data->style->dpi);
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        message == WM_KEYDOWN || message == WM_CHAR || message == WM_VSCROLL || message == WM_MOUSEWHEEL ||
        message == LB_SETTOPINDEX || message == LB_SETCURSEL || message == LB_ADDSTRING ||
        message == LB_DELETESTRING || message == LB_RESETCONTENT || message == WM_SIZE || message == WM_SHOWWINDOW) {
        // Native ComboLBox can draw changed rows synchronously, including
        // owner-draw notifications. Replace that paint with the same palette.
        InvalidateRect(list, nullptr, FALSE);
        if (IsWindowVisible(list))
            UpdateWindow(list);
    }
    return result;
}
inline void UiPrepareComboPopup(HWND combo, UiFieldStyle *style) {
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(combo, &info) || !info.hwndList)
        return;
    DWORD_PTR reference = 0;
    if (GetWindowSubclass(info.hwndList, UiComboPopupProc, 72, &reference)) {
        reinterpret_cast<UiComboPopup *>(reference)->style = style;
    } else {
        auto *data = new (std::nothrow) UiComboPopup{combo, style};
        if (!data)
            return;
        if (!SetWindowSubclass(info.hwndList, UiComboPopupProc, 72, reinterpret_cast<DWORD_PTR>(data))) {
            delete data;
            return;
        }
        SetWindowLongPtrW(info.hwndList, GWL_STYLE, GetWindowLongPtrW(info.hwndList, GWL_STYLE) & ~(WS_BORDER | WS_VSCROLL | WS_HSCROLL));
        SetWindowLongPtrW(info.hwndList, GWL_EXSTYLE, GetWindowLongPtrW(info.hwndList, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(info.hwndList, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    HDC dc = GetDC(combo);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0));
    HGDIOBJ previousFont = dc && font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    if (dc)
        GetTextMetricsW(dc, &metrics);
    if (previousFont)
        SelectObject(dc, previousFont);
    if (dc)
        ReleaseDC(combo, dc);
    const int row = std::max(MulDiv(28, style->dpi, 96), static_cast<int>(metrics.tmHeight) + MulDiv(8, style->dpi, 96));
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, row);
    UiComboRegion(info.hwndList, style->dpi);
}
inline void UiResizeDialog(HWND dialog, int dpi, int width, int height) {
    RECT bounds{0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)};
    AdjustWindowRectEx(&bounds, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
    SetWindowPos(dialog, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
inline LRESULT CALLBACK UiButtonProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (message == WM_ERASEBKGND && (GetWindowLongPtrW(h, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
        return 1;
    if (message == WM_MOUSEMOVE && !GetPropW(h, L"SerialCtl.PowerHover")) {
        SetPropW(h, L"SerialCtl.PowerHover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
        TrackMouseEvent(&track);
        InvalidateRect(h, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE) {
        RemovePropW(h, L"SerialCtl.PowerHover");
        InvalidateRect(h, nullptr, FALSE);
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(h, L"SerialCtl.PowerHover");
        RemoveWindowSubclass(h, UiButtonProc, id);
    }
    return DefSubclassProc(h, message, w, l);
}
inline void UiStyleButton(HWND h) {
    SetWindowSubclass(h, UiButtonProc, 81, 0);
}
inline void UiPaintCheckBox(HWND h, HDC target, const UiFieldStyle &style) {
    RECT bounds{};
    GetClientRect(h, &bounds);
    if (!target || bounds.right <= 0 || bounds.bottom <= 0)
        return;
    const auto colors = UiTheme(*style.dark);
    auto d = [&](int value) { return MulDiv(value, style.dpi, 96); };
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, bounds.right, bounds.bottom) : nullptr;
    HGDIOBJ previousBitmap = bitmap ? SelectObject(buffer, bitmap) : nullptr;
    HDC dc = bitmap ? buffer : target;
    const int saved = SaveDC(dc);
    const COLORREF outside = GetPropW(h, L"SerialCtl.CheckSurface") ? colors.surface : colors.field;
    HBRUSH background = CreateSolidBrush(outside);
    FillRect(dc, &bounds, background);
    DeleteObject(background);
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const auto state = SendMessageW(h, BM_GETSTATE, 0, 0);
    const auto checked = SendMessageW(h, BM_GETCHECK, 0, 0);
    const auto uiState = SendMessageW(h, WM_QUERYUISTATE, 0, 0);
    const bool focused = enabled && (state & BST_FOCUS) && !(uiState & UISF_HIDEFOCUS);
    const bool pressed = enabled && (state & BST_PUSHED);
    const bool hovered = enabled && GetPropW(h, L"SerialCtl.CheckHover");
    const int size = std::min(d(16), static_cast<int>(bounds.bottom));
    RECT mark{0, (bounds.bottom - size) / 2, size, (bounds.bottom + size) / 2};
    const COLORREF border = !enabled ? colors.border : focused ? colors.accent : hovered ? colors.muted : colors.border;
    const COLORREF fill = !enabled ? colors.raised : pressed ? colors.accentSoft :
        checked != BST_UNCHECKED ? colors.accent : hovered ? colors.raised : colors.field;
    UiBox(dc, mark, fill, checked != BST_UNCHECKED && enabled && !pressed ? colors.accent : border, d(2));
    const COLORREF ink = enabled && !pressed ? RGB(255, 255, 255) : colors.muted;
    if (checked == BST_CHECKED) {
        const POINT tick[]{{mark.left + d(4), mark.top + d(8)},
                           {mark.left + d(7), mark.top + d(11)},
                           {mark.left + d(12), mark.top + d(5)}};
        UiPolyline(dc, tick, 3, ink, static_cast<float>(std::max(1, d(2))));
    } else if (checked == BST_INDETERMINATE) {
        const POINT dash[]{{mark.left + d(4), mark.top + d(8)}, {mark.right - d(4), mark.top + d(8)}};
        UiPolyline(dc, dash, 2, ink, static_cast<float>(std::max(1, d(2))));
    }
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
    if (font || style.font)
        SelectObject(dc, font ? font : style.font);
    wchar_t label[512]{};
    GetWindowTextW(h, label, static_cast<int>(std::size(label)));
    RECT textBounds = bounds;
    textBounds.left = mark.right + d(8);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? colors.text : colors.muted);
    const UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
        ((uiState & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
    // DrawText, unlike the native disabled BUTTON painter, does not add a
    // light embossed shadow that becomes conspicuous on the dark surface.
    DrawTextW(dc, label, -1, &textBounds, format);
    if (focused) {
        RECT focus = textBounds;
        DrawTextW(dc, label, -1, &focus, DT_SINGLELINE | DT_CALCRECT |
            ((uiState & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0));
        const int height = focus.bottom - focus.top;
        focus.top = (bounds.bottom - height) / 2;
        focus.bottom = focus.top + height;
        focus.right = std::min(focus.right + d(2), bounds.right);
        SetBkColor(dc, outside);
        DrawFocusRect(dc, &focus);
    }
    RestoreDC(dc, saved);
    if (bitmap) {
        BitBlt(target, 0, 0, bounds.right, bounds.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previousBitmap);
        DeleteObject(bitmap);
    }
    if (buffer)
        DeleteDC(buffer);
}
inline LRESULT CALLBACK UiCheckBoxProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR reference) {
    auto *style = reinterpret_cast<UiFieldStyle *>(reference);
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT || message == WM_PRINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
        UiPaintCheckBox(h, dc, *style);
        if (message == WM_PAINT)
            EndPaint(h, &ps);
        return 0;
    }
    if (message == WM_MOUSEMOVE && !GetPropW(h, L"SerialCtl.CheckHover")) {
        SetPropW(h, L"SerialCtl.CheckHover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
        TrackMouseEvent(&track);
    }
    if (message == WM_MOUSELEAVE)
        RemovePropW(h, L"SerialCtl.CheckHover");
    if (message == WM_NCDESTROY) {
        RemovePropW(h, L"SerialCtl.CheckHover");
        RemovePropW(h, L"SerialCtl.CheckSurface");
        RemoveWindowSubclass(h, UiCheckBoxProc, id);
        return DefSubclassProc(h, message, w, l);
    }
    // The native BS_AUTOCHECKBOX still owns check state, keyboard/mouse input,
    // focus, BN_CLICKED and accessibility; never toggle a second time here.
    const LRESULT result = DefSubclassProc(h, message, w, l);
    DWORD_PTR current = 0;
    if (!GetWindowSubclass(h, UiCheckBoxProc, id, &current) || current != reference)
        return result; // A native notification may have destroyed its dialog.
    if (message == BM_SETCHECK || message == BM_SETSTATE || message == BM_CLICK ||
        message == WM_ENABLE || message == WM_SETTEXT || message == WM_SETFONT ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_SIZE ||
        message == WM_MOUSEMOVE || message == WM_MOUSELEAVE || message == WM_LBUTTONDOWN ||
        message == WM_LBUTTONUP || message == WM_KEYDOWN || message == WM_KEYUP ||
        message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_UPDATEUISTATE ||
        message == WM_THEMECHANGED || message == WM_SYSCOLORCHANGE)
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    return result;
}
inline void UiStyleCheckBox(HWND control, UiFieldStyle *style, bool surface) {
    if (!SetWindowSubclass(control, UiCheckBoxProc, 82, reinterpret_cast<DWORD_PTR>(style)))
        return;
    if (surface)
        SetPropW(control, L"SerialCtl.CheckSurface", reinterpret_cast<HANDLE>(1));
    else
        RemovePropW(control, L"SerialCtl.CheckSurface");
    InvalidateRect(control, nullptr, FALSE);
}
inline LRESULT CALLBACK UiFieldProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR reference) {
    auto *style = reinterpret_cast<UiFieldStyle *>(reference);
    wchar_t cls[32]{};
    GetClassNameW(h, cls, 32);
    bool edit = _wcsicmp(cls, L"EDIT") == 0;
    bool hoverChanged = false;
    auto d = [style](int n) { return MulDiv(n, style->dpi, 96); };
    auto colors = UiTheme(*style->dark);
    if (!edit && (message == WM_NCCALCSIZE || message == WM_NCPAINT))
        return 0;
    if (!edit && message == WM_ERASEBKGND)
        return 1;
    if (!edit && (message == WM_PAINT || message == WM_PRINT || message == WM_PRINTCLIENT)) {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
        UiPaintComboField(h, dc, *style);
        if (message == WM_PAINT)
            EndPaint(h, &ps);
        return 0;
    }
    if (!edit && (message == WM_MOUSEMOVE || message == WM_LBUTTONUP)) {
        COMBOBOXINFO info{sizeof(info)};
        DWORD_PTR popup = 0;
        if (GetComboBoxInfo(h, &info) &&
            GetWindowSubclass(info.hwndList, UiComboPopupProc, 72, &popup) &&
            reinterpret_cast<UiComboPopup *>(popup)->dragging) {
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            MapWindowPoints(h, info.hwndList, &point, 1);
            return SendMessageW(info.hwndList, message, w, MAKELPARAM(point.x, point.y));
        }
    }
    if (!edit && message == WM_MOUSEMOVE && !GetPropW(h, L"SerialCtl.FieldHover")) {
        hoverChanged = true;
        SetPropW(h, L"SerialCtl.FieldHover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
        TrackMouseEvent(&track);
        InvalidateRect(h, nullptr, FALSE);
    }
    if (!edit && message == WM_MOUSELEAVE) {
        hoverChanged = GetPropW(h, L"SerialCtl.FieldHover") != nullptr;
        RemovePropW(h, L"SerialCtl.FieldHover");
        InvalidateRect(h, nullptr, FALSE);
    }
    if (!edit && (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_KEYDOWN ||
                 (message == CB_SHOWDROPDOWN && w)))
        UiPrepareComboPopup(h, style);
    if (edit && message == WM_NCCALCSIZE) {
        LRESULT result = DefSubclassProc(h, message, w, l);
        RECT *rect = w ? &reinterpret_cast<NCCALCSIZE_PARAMS *>(l)->rgrc[0] : reinterpret_cast<RECT *>(l);
        InflateRect(rect, -d(8), -d(4));
        return result;
    }
    wchar_t parentClass[32]{};
    GetClassNameW(GetParent(h), parentClass, 32);
    const COLORREF outsideColor =
        GetPropW(h, L"SerialCtl.FieldSurface") || _wcsicmp(parentClass, L"#32770") == 0 ? colors.surface : colors.field;
    if (edit && message == WM_PRINT) {
        HDC dc = reinterpret_cast<HDC>(w);
        int saved = SaveDC(dc);
        RECT bounds{};
        GetWindowRect(h, &bounds);
        OffsetRect(&bounds, -bounds.left, -bounds.top);
        HBRUSH outside = CreateSolidBrush(outsideColor);
        FillRect(dc, &bounds, outside);
        DeleteObject(outside);
        UiBox(dc, bounds, colors.field, GetFocus() == h ? colors.accent : colors.border, d(10));
        POINT origin{};
        GetViewportOrgEx(dc, &origin);
        SetViewportOrgEx(dc, origin.x + d(8), origin.y + d(4), nullptr);
        IntersectClipRect(dc, 0, 0, bounds.right - d(16), bounds.bottom - d(8));
        DefSubclassProc(h, WM_PRINTCLIENT, w, PRF_CLIENT | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
        return 0;
    }
    if (edit && message == WM_NCPAINT) {
        HDC dc = GetWindowDC(h);
        const int saved = SaveDC(dc);
        RECT rect{};
        GetWindowRect(h, &rect);
        OffsetRect(&rect, -rect.left, -rect.top);
        ExcludeClipRect(dc, d(8), d(4), rect.right - d(8), rect.bottom - d(4));
        HBRUSH outside = CreateSolidBrush(outsideColor);
        FillRect(dc, &rect, outside);
        DeleteObject(outside);
        const bool enabled = IsWindowEnabled(h) != FALSE;
        const bool focused = enabled && GetFocus() == h;
        const COLORREF border = focused ? colors.accent
                                  : enabled && GetPropW(h, L"SerialCtl.FieldHover") ? colors.muted : colors.border;
        UiBox(dc, rect, colors.field, border, d(10));
        RestoreDC(dc, saved);
        ReleaseDC(h, dc);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(h, L"SerialCtl.FieldSurface");
        RemovePropW(h, L"SerialCtl.FieldHover");
        RemoveWindowSubclass(h, UiFieldProc, id);
        return DefSubclassProc(h, message, w, l);
    }
    LRESULT result = DefSubclassProc(h, message, w, l);
    DWORD_PTR current = 0;
    if (!GetWindowSubclass(h, UiFieldProc, id, &current) || current != reference)
        return result;
    if (!edit && message == WM_SETFONT)
        UiPrepareComboPopup(h, style);
    if (!edit && (hoverChanged || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE ||
                  message == WM_SETFONT || message == WM_SIZE || message == WM_KEYDOWN || message == WM_KEYUP ||
                  message == WM_CHAR || message == WM_COMMAND || message == WM_MOUSEWHEEL ||
                  message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK ||
                  message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_UPDATEUISTATE ||
                  message == WM_THEMECHANGED || message == WM_SYSCOLORCHANGE || message == CB_SETCURSEL ||
                  message == CB_SELECTSTRING || message == CB_SHOWDROPDOWN || message == CB_RESETCONTENT ||
                  message == CB_DELETESTRING || (message == CB_SETITEMHEIGHT && static_cast<int>(w) == -1))) {
        // Native keyboard/list notifications also redraw through GetDC. Commit
        // one complete themed field before returning, not only after focus loss.
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    } else if (edit && (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE ||
                        message == WM_SETFONT || message == WM_SIZE))
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    return result;
}
inline void UiStyleField(HWND control, UiFieldStyle *style) {
    SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) & ~WS_BORDER);
    SetWindowLongPtrW(control, GWL_EXSTYLE, GetWindowLongPtrW(control, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SetWindowSubclass(control, UiFieldProc, 71, reinterpret_cast<DWORD_PTR>(style));
    // The inset native edit surface cannot cover the rounded border. Paint
    // outside corners in the parent surface instead of clipping away AA pixels.
    SetWindowRgn(control, nullptr, FALSE);
    SetWindowPos(control, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    wchar_t cls[32]{};
    GetClassNameW(control, cls, 32);
    if (_wcsicmp(cls, L"COMBOBOX") == 0)
        UiPrepareComboPopup(control, style);
}
} // namespace serialctl

namespace serialctl {
// Restore only the owner of a modal dismissed while that modal was foreground.
// One restoration per dialog; never topmost and never from repaint/timer paths.
inline void UiEndOwnedDialog(HWND dialog, INT_PTR result) {
    HWND owner = GetWindow(dialog, GW_OWNER);
    if (owner && GetForegroundWindow() == dialog)
        SetPropW(owner, L"SerialCtl.RestoreModal", reinterpret_cast<HANDLE>(1));
    EndDialog(dialog, result);
}
class UiModalOwner {
  public:
    explicit UiModalOwner(HWND owner) : owner_(owner), focus_(GetFocus()) {
        RemovePropW(owner_, L"SerialCtl.RestoreModal");
    }
    ~UiModalOwner() {
        if (!IsWindow(owner_) || !RemovePropW(owner_, L"SerialCtl.RestoreModal"))
            return;
        SetActiveWindow(owner_);
        if (GetForegroundWindow() != owner_)
            SetForegroundWindow(owner_);
        if (IsWindow(focus_) && (focus_ == owner_ || IsChild(owner_, focus_)))
            SetFocus(focus_);
    }

  private:
    HWND owner_, focus_;
};
inline INT_PTR UiDialogBoxOwned(HINSTANCE instance, LPCWSTR resource, HWND owner, DLGPROC proc, LPARAM parameter) {
    UiModalOwner restore(owner);
    return DialogBoxParamW(instance, resource, owner, proc, parameter);
}
inline INT_PTR UiDialogBoxIndirectOwned(HINSTANCE instance, LPCDLGTEMPLATE resource, HWND owner, DLGPROC proc,
                                        LPARAM parameter) {
    UiModalOwner restore(owner);
    return DialogBoxIndirectParamW(instance, resource, owner, proc, parameter);
}
// Collect final child bounds/visibility, then commit one native layout transaction.
// If a native batch allocation fails, apply all final bounds individually.
class UiLayoutBatch {
  public:
    void Move(HWND h, int x, int y, int w, int height, BOOL = FALSE) {
        auto &v = items_[h];
        v.rect = {x, y, x + w, y + height};
        v.move = true;
    }
    void Show(HWND h, int show) {
        items_[h].show = show == SW_HIDE ? 0 : 1;
    }
    void ComboVisibleHeight(HWND h, int height) {
        items_[h].comboHeight = height;
    }
    void Commit() {
        struct Move {
            HWND h;
            RECT rect;
            UINT flags;
        };
        std::vector<Move> changes;
        std::vector<std::pair<HWND, int>> combos;
        for (auto &p : items_) {
            auto h = p.first;
            auto &v = p.second;
            if (!h)
                continue;
            if (v.comboHeight >= 0)
                combos.push_back({h, v.comboHeight});
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE;
            bool different = false;
            if (v.move) {
                RECT r{};
                GetWindowRect(h, &r);
                MapWindowPoints(HWND_DESKTOP, GetParent(h), reinterpret_cast<POINT *>(&r), 2);
                // ComboBox window height is the closed field, while the requested
                // layout height also reserves its dropdown. Compare position/width
                // here and commit the visible height after the native move.
                if (v.comboHeight >= 0)
                    r.bottom = v.rect.bottom;
                if (!EqualRect(&r, &v.rect)) {
                    flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
                    // An owner-drawn control's rounded exterior depends on its
                    // final dimensions. Do not copy old edge pixels into the new
                    // rectangle while the full repaint is pending.
                    wchar_t className[16]{};
                    GetClassNameW(h, className, 16);
                    const auto style = GetWindowLongPtrW(h, GWL_STYLE);
                    if ((_wcsicmp(className, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_OWNERDRAW) ||
                        (_wcsicmp(className, L"LISTBOX") == 0 &&
                         (style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE))))
                        flags |= SWP_NOCOPYBITS;
                    different = true;
                }
            }
            bool visible = (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0;
            if (v.show >= 0 && visible != (v.show != 0)) {
                flags |= v.show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
                different = true;
            }
            if (different)
                changes.push_back({h, v.rect, flags});
        }
        items_.clear();
        HDWP batch = changes.empty() ? nullptr : BeginDeferWindowPos(static_cast<int>(changes.size()));
        for (auto &c : changes) {
            if (!batch)
                break;
            batch = DeferWindowPos(batch, c.h, nullptr, c.rect.left, c.rect.top, c.rect.right - c.rect.left,
                                   c.rect.bottom - c.rect.top, c.flags);
        }
        bool applied = batch && EndDeferWindowPos(batch);
        if (!applied)
            for (auto &c : changes)
                SetWindowPos(c.h, nullptr, c.rect.left, c.rect.top, c.rect.right - c.rect.left,
                             c.rect.bottom - c.rect.top, c.flags);
        for (auto &c : changes)
            InvalidateRect(c.h, nullptr, FALSE);
        for (auto &combo : combos) {
            RECT bounds{};
            GetWindowRect(combo.first, &bounds);
            int visible = bounds.bottom - bounds.top;
            if (visible != combo.second) {
                auto selection = static_cast<WPARAM>(-1);
                int current = static_cast<int>(SendMessageW(combo.first, CB_GETITEMHEIGHT, selection, 0));
                SendMessageW(combo.first, CB_SETITEMHEIGHT, selection, std::max(1, current + combo.second - visible));
                InvalidateRect(combo.first, nullptr, FALSE);
            }
        }
    }
    ~UiLayoutBatch() {
        Commit();
    }

  private:
    struct Item {
        RECT rect{};
        bool move = false;
        int comboHeight = -1;
        int show = -1;
    };
    std::map<HWND, Item> items_;
};
inline void UiPlus(HDC dc, const RECT &r, COLORREF color, int dpi) {
    int half = MulDiv(7, dpi, 96), width = std::max(1, MulDiv(2, dpi, 96));
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const POINT horizontal[]{{cx - half, cy}, {cx + half, cy}};
    const POINT vertical[]{{cx, cy - half}, {cx, cy + half}};
    UiPolyline(dc, horizontal, 2, color, static_cast<float>(width));
    UiPolyline(dc, vertical, 2, color, static_cast<float>(width));
}
} // namespace serialctl
