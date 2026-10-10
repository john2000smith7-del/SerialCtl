#pragma once
#include <algorithm>
#include <commctrl.h>
#include <map>
#include <string>
#include <vector>
#include <windows.h>
namespace serialctl {
struct UiColors {
    COLORREF surface, raised, field, text, muted, border, accent, danger;
};
inline UiColors UiTheme(bool dark) {
    return dark ? UiColors{RGB(36, 36, 38),    RGB(44, 44, 46), RGB(20, 20, 22),   RGB(245, 245, 247),
                           RGB(152, 152, 157), RGB(58, 58, 60), RGB(10, 132, 255), RGB(255, 69, 58)}
                : UiColors{RGB(255, 255, 255), RGB(242, 242, 247), RGB(255, 255, 255), RGB(29, 29, 31),
                           RGB(110, 110, 115), RGB(210, 210, 215), RGB(0, 122, 255),   RGB(255, 59, 48)};
}
inline void UiBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}
struct UiFieldStyle {
    bool *dark;
    HFONT font;
    int dpi;
};
inline void UiResizeDialog(HWND dialog, int dpi, int width, int height) {
    RECT bounds{0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)};
    AdjustWindowRectEx(&bounds, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
    SetWindowPos(dialog, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
inline LRESULT CALLBACK UiButtonProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
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
inline LRESULT CALLBACK UiFieldProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR reference) {
    auto *style = reinterpret_cast<UiFieldStyle *>(reference);
    wchar_t cls[32]{};
    GetClassNameW(h, cls, 32);
    bool edit = _wcsicmp(cls, L"EDIT") == 0;
    auto d = [style](int n) { return MulDiv(n, style->dpi, 96); };
    auto colors = UiTheme(*style->dark);
    if (!edit && message == WM_NCCALCSIZE)
        return 0;
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
    if ((edit && message == WM_NCPAINT) ||
        (!edit && (message == WM_PAINT || message == WM_PRINT || message == WM_PRINTCLIENT))) {
        PAINTSTRUCT ps{};
        HDC dc = edit ? GetWindowDC(h) : message == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
        RECT rect{};
        if (edit) {
            GetWindowRect(h, &rect);
            OffsetRect(&rect, -rect.left, -rect.top);
            ExcludeClipRect(dc, d(8), d(4), rect.right - d(8), rect.bottom - d(4));
        } else
            GetClientRect(h, &rect);
        HBRUSH outside = CreateSolidBrush(outsideColor);
        FillRect(dc, &rect, outside);
        DeleteObject(outside);
        UiBox(dc, rect, colors.field, GetFocus() == h ? colors.accent : colors.border, d(10));
        if (!edit) {
            int selected = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
            std::wstring text;
            if (selected >= 0) {
                int length = static_cast<int>(SendMessageW(h, CB_GETLBTEXTLEN, selected, 0));
                if (length >= 0 && length < 1024) {
                    std::vector<wchar_t> buffer(length + 1);
                    SendMessageW(h, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(buffer.data()));
                    text.assign(buffer.data());
                }
            }
            auto old = SelectObject(dc, style->font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, IsWindowEnabled(h) ? colors.text : colors.muted);
            RECT label = rect;
            label.left += d(8);
            label.right -= d(28);
            DrawTextW(dc, text.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            SelectObject(dc, old);
            HPEN pen = CreatePen(PS_SOLID, d(2), colors.muted);
            old = SelectObject(dc, pen);
            int x = rect.right - d(14), y = rect.bottom / 2;
            MoveToEx(dc, x - d(4), y - d(2), nullptr);
            LineTo(dc, x, y + d(2));
            LineTo(dc, x + d(4), y - d(2));
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        if (edit)
            ReleaseDC(h, dc);
        else if (message == WM_PAINT)
            EndPaint(h, &ps);
        return 0;
    }
    LRESULT result = DefSubclassProc(h, message, w, l);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    if (message == WM_SIZE) {
        RECT bounds{};
        GetWindowRect(h, &bounds);
        SetWindowRgn(
            h, CreateRoundRectRgn(0, 0, bounds.right - bounds.left + 1, bounds.bottom - bounds.top + 1, d(20), d(20)),
            TRUE);
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(h, L"SerialCtl.FieldSurface");
        RemoveWindowSubclass(h, UiFieldProc, id);
    }
    return result;
}
inline void UiStyleField(HWND control, UiFieldStyle *style) {
    SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) & ~WS_BORDER);
    SetWindowLongPtrW(control, GWL_EXSTYLE, GetWindowLongPtrW(control, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SetWindowSubclass(control, UiFieldProc, 71, reinterpret_cast<DWORD_PTR>(style));
    SetWindowPos(control, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
    void Commit() {
        struct Move {
            HWND h;
            RECT rect;
            UINT flags;
        };
        std::vector<Move> changes;
        for (auto &p : items_) {
            auto h = p.first;
            auto &v = p.second;
            if (!h)
                continue;
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE;
            bool different = false;
            if (v.move) {
                RECT r{};
                GetWindowRect(h, &r);
                MapWindowPoints(HWND_DESKTOP, GetParent(h), reinterpret_cast<POINT *>(&r), 2);
                if (!EqualRect(&r, &v.rect)) {
                    flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
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
        if (changes.empty())
            return;
        HDWP batch = BeginDeferWindowPos(static_cast<int>(changes.size()));
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
    }
    ~UiLayoutBatch() {
        Commit();
    }

  private:
    struct Item {
        RECT rect{};
        bool move = false;
        int show = -1;
    };
    std::map<HWND, Item> items_;
};
inline void UiPlus(HDC dc, const RECT &r, COLORREF color, int dpi) {
    int half = MulDiv(7, dpi, 96), width = std::max(1, MulDiv(2, dpi, 96));
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, width, color);
    auto old = SelectObject(dc, pen);
    MoveToEx(dc, cx - half, cy, nullptr);
    LineTo(dc, cx + half + 1, cy);
    MoveToEx(dc, cx, cy - half, nullptr);
    LineTo(dc, cx, cy + half + 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}
} // namespace serialctl
