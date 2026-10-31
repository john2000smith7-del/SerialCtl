#pragma once
#include "UiStyle.h"
#include <initializer_list>

namespace serialctl {
// Keep native dialog children/IDs/tab order. Only overflow content scrolls;
// footer actions stay visible, and child regions cannot paint over that footer.
class UiDialogViewport {
    struct Item { HWND window; RECT bounds; bool footer; };
    HWND window_;
    int dpi_, offset_ = 0, maximum_ = 0, viewport_ = 0, content_ = 0, grab_ = 0;
    bool dragging_ = false;
    SIZE naturalSize_{};
    std::vector<Item> items_;
    int D(int value) const { return MulDiv(value, dpi_, 96); }
    RECT Thumb() const {
        RECT r{}; GetClientRect(window_, &r);
        const int height = std::min(viewport_, std::max(D(28), viewport_ * viewport_ / std::max(1, content_)));
        const int y = maximum_ ? offset_ * (viewport_ - height) / maximum_ : 0;
        return {r.right - D(8), y, r.right - D(3), y + height};
    }
    void PaintThumb(HDC dc) const {
        const auto thumb = Thumb();
        // Use the dialog's own theme rather than a system scrollbar track.
        auto brush = reinterpret_cast<HBRUSH>(SendMessageW(window_, WM_CTLCOLORDLG,
                                                          reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(window_)));
        LOGBRUSH color{};
        const bool dark = brush && GetObjectW(brush, sizeof(color), &color) && GetRValue(color.lbColor) < 128;
        UiBox(dc, thumb, UiTheme(dark).border, UiTheme(dark).border, D(3));
    }
    void Scroll(int position) {
        offset_ = std::clamp(position, 0, maximum_);
        UiRedrawScope redraw(window_);
        RECT client{}; GetClientRect(window_, &client);
        for (const auto &item : items_) {
            if (item.footer || !IsWindow(item.window)) continue;
            const int naturalY = item.bounds.top - offset_;
            const int height = item.bounds.bottom - item.bounds.top;
            const bool outside = naturalY >= viewport_ || naturalY + height <= 0;
            // Keep offscreen children visible in the native Tab order. An
            // empty window region can still leak direct native painting into
            // the footer (notably under Wine); park it outside the client.
            const int y = naturalY >= viewport_ ? client.bottom + height :
                          naturalY + height <= 0 ? -height - 1 : naturalY;
            SetWindowPos(item.window, nullptr, item.bounds.left, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            const int top = std::clamp(-naturalY, 0, height), bottom = std::clamp(viewport_ - naturalY, top, height);
            HRGN clip = outside || (top == 0 && bottom == height) ? nullptr :
                CreateRectRgn(0, top, item.bounds.right - item.bounds.left, bottom);
            SetWindowRgn(item.window, clip, FALSE);
        }
    }
    void Reveal(HWND control) {
        if (HWND frame = reinterpret_cast<HWND>(GetPropW(control, L"SerialCtl.FieldFrame"))) control = frame;
        for (const auto &item : items_) {
            if (item.window != control || item.footer) continue;
            if (item.bounds.top < offset_) Scroll(item.bounds.top);
            else if (item.bounds.bottom > offset_ + viewport_)
                Scroll(item.bounds.bottom - viewport_);
            break;
        }
    }
    static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto self = reinterpret_cast<UiDialogViewport*>(data);
        if (m == WM_SETFOCUS) self->Reveal(h);
        if (m == WM_MOUSEWHEEL) {
            wchar_t name[24]{}; GetClassNameW(h, name, 24);
            if (_wcsicmp(name, L"COMBOBOX") != 0 || !SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0))
                return SendMessageW(self->window_, m, w, l);
        }
        if (m == WM_NCDESTROY) RemoveWindowSubclass(h, ChildProc, id);
        return DefSubclassProc(h, m, w, l);
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto self = reinterpret_cast<UiDialogViewport*>(data);
        if (m == WM_MOUSEWHEEL) {
            self->Scroll(self->offset_ - GET_WHEEL_DELTA_WPARAM(w) * self->D(40) / WHEEL_DELTA);
            return 0;
        }
        if (m == WM_LBUTTONDOWN) {
            RECT r{}; GetClientRect(h, &r);
            if (GET_X_LPARAM(l) >= r.right - self->D(16) && GET_Y_LPARAM(l) < self->viewport_) {
                const auto thumb = self->Thumb();
                self->dragging_ = true;
                self->grab_ = std::clamp(static_cast<int>(GET_Y_LPARAM(l) - thumb.top), 0, static_cast<int>(thumb.bottom - thumb.top));
                SetCapture(h);
                SendMessageW(h, WM_MOUSEMOVE, w, l);
                return 0;
            }
        }
        if (m == WM_MOUSEMOVE && self->dragging_) {
            const auto thumb = self->Thumb();
            self->Scroll(static_cast<int>(static_cast<long long>(GET_Y_LPARAM(l) - self->grab_) * self->maximum_ /
                         std::max(1, self->viewport_ - static_cast<int>(thumb.bottom - thumb.top))));
            return 0;
        }
        if (m == WM_LBUTTONUP && self->dragging_) {
            self->dragging_ = false; ReleaseCapture(); return 0;
        }
        if (m == WM_CAPTURECHANGED) self->dragging_ = false;
        if (m == WM_PAINT || m == WM_PRINTCLIENT) {
            const auto result = DefSubclassProc(h, m, w, l);
            HDC dc = m == WM_PAINT ? GetDC(h) : reinterpret_cast<HDC>(w);
            if (dc) self->PaintThumb(dc);
            if (m == WM_PAINT && dc) ReleaseDC(h, dc);
            return result;
        }
        if (m == WM_NCDESTROY) {
            for (const auto &item : self->items_)
                if (IsWindow(item.window)) RemoveWindowSubclass(item.window, ChildProc, 86);
            RemoveWindowSubclass(h, Proc, id);
            delete self;
        }
        return DefSubclassProc(h, m, w, l);
    }
  public:
    // Restore unscrolled geometry before re-laying out a dynamic dialog. The
    // caller can wrap Reset/layout/Fit in one UiRedrawScope to avoid a flash.
    static void Reset(HWND dialog) {
        DWORD_PTR data = 0;
        if (!GetWindowSubclass(dialog, Proc, 86, &data)) return;
        auto self = reinterpret_cast<UiDialogViewport *>(data);
        if (GetCapture() == dialog) ReleaseCapture();
        for (const auto &item : self->items_) {
            if (!IsWindow(item.window)) continue;
            RemoveWindowSubclass(item.window, ChildProc, 86);
            SetWindowRgn(item.window, nullptr, FALSE);
            SetWindowPos(item.window, nullptr, item.bounds.left, item.bounds.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
        RemoveWindowSubclass(dialog, Proc, 86);
        SetWindowPos(dialog, nullptr, 0, 0, self->naturalSize_.cx, self->naturalSize_.cy,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        delete self;
    }
    static void Fit(HWND dialog, int dpi, std::initializer_list<int> footerIds = {IDCANCEL, IDOK}) {
        SERIALCTL_DIALOG_TRACE_SCOPE(Viewport);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        RECT bounds{}, client{}; GetWindowRect(dialog, &bounds); GetClientRect(dialog, &client);
        const int margin = MulDiv(12, dpi, 96);
        const int height = std::min(static_cast<int>(bounds.bottom - bounds.top),
                                   static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - margin * 2);
        auto self = new UiDialogViewport;
        self->window_ = dialog; self->dpi_ = dpi;
        self->naturalSize_ = {bounds.right - bounds.left, bounds.bottom - bounds.top};
        int footerTop = INT_MAX, footerBottom = 0;
        int naturalClient = client.bottom;
        for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT r{}; GetWindowRect(child, &r); MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT*>(&r), 2);
            const bool footer = std::find(footerIds.begin(), footerIds.end(), GetDlgCtrlID(child)) != footerIds.end();
            self->items_.push_back({child, r, footer});
            naturalClient = std::max(naturalClient, static_cast<int>(r.bottom) + MulDiv(16, dpi, 96));
            if (footer) { footerTop = std::min(footerTop, static_cast<int>(r.top)); footerBottom = std::max(footerBottom, static_cast<int>(r.bottom)); }
        }
        if (!footerBottom) { delete self; return; }
        if (height >= bounds.bottom - bounds.top && naturalClient <= client.bottom) { delete self; return; }
        // Credits below the action row belong to the fixed footer as well.
        for (auto &item : self->items_) {
            item.footer = item.footer || item.bounds.top >= footerTop;
            if (!item.footer) self->content_ = std::max(self->content_, static_cast<int>(item.bounds.bottom));
        }
        UiRedrawScope redraw(dialog);
        const int newClient = height - (bounds.bottom - bounds.top - client.bottom);
        // Windows may have capped the initial outer window before children
        // were placed. Their natural content extent still determines the footer.
        const int footerShift = newClient - naturalClient;
        self->viewport_ = std::max(1, footerTop + footerShift - margin);
        self->content_ += margin;
        self->maximum_ = std::max(0, self->content_ - self->viewport_);
        const int left = std::clamp(static_cast<int>(bounds.left), static_cast<int>(monitor.rcWork.left) + margin,
                                  std::max(static_cast<int>(monitor.rcWork.left) + margin,
                                           static_cast<int>(monitor.rcWork.right - (bounds.right - bounds.left)) - margin));
        SetWindowPos(dialog, nullptr, left, monitor.rcWork.top + margin, bounds.right - bounds.left, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (!SetWindowSubclass(dialog, Proc, 86, reinterpret_cast<DWORD_PTR>(self))) { delete self; return; }
        for (const auto &item : self->items_) {
            if (item.footer)
                SetWindowPos(item.window, nullptr, item.bounds.left, item.bounds.top + footerShift, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            else SetWindowSubclass(item.window, ChildProc, 86, reinterpret_cast<DWORD_PTR>(self));
        }
        self->Scroll(0);
    }
};
} // namespace serialctl
