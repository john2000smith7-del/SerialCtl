#pragma once
#include "UiStyle.h"

namespace serialctl {
// A compact path disclosure. The value is always the full path; display and
// horizontal scrolling never alter it. Double-click/Enter asks the owner to edit.
class UiPathField {
    struct Data {
        UiFieldStyle *style;
        std::wstring value;
        bool expanded = false, dragging = false;
        int offset = 0, maximum = 0, grab = 0;
    };
    static int D(const Data &d, int n) {
        return MulDiv(n, d.style->dpi, 96);
    }
    static RECT Thumb(HWND h, Data &d) {
        RECT r{};
        GetClientRect(h, &r);
        const int width = std::max(1L, r.right - D(d, 16));
        const int size = std::min(width, std::max(D(d, 28), width * width / std::max(1, width + d.maximum)));
        const int x = D(d, 8) + (d.maximum ? d.offset * (width - size) / d.maximum : 0);
        return {x, r.bottom - D(d, 8), x + size, r.bottom - D(d, 3)};
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto d = reinterpret_cast<Data *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            d = new (std::nothrow)
                Data{static_cast<UiFieldStyle *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams)};
            if (!d)
                return FALSE;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        }
        if (!d)
            return DefWindowProcW(h, m, w, l);
        if (m == WM_SETTEXT) {
            const std::wstring value = l ? reinterpret_cast<const wchar_t *>(l) : L"";
            if (value != d->value) {
                d->value = value;
                d->offset = 0;
                d->expanded = false;
            }
            InvalidateRect(h, nullptr, FALSE);
            return DefWindowProcW(h, m, w, l);
        }
        if (m == WM_SETFONT || m == WM_SIZE || m == WM_ENABLE)
            InvalidateRect(h, nullptr, FALSE);
        if (m == WM_ERASEBKGND)
            return 1;
        if (m == WM_GETDLGCODE)
            return DLGC_WANTARROWS | ((w == VK_RETURN || w == VK_F2) ? DLGC_WANTMESSAGE : 0);
        if (m == WM_KEYDOWN && w == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (OpenClipboard(h)) {
                const size_t bytes = (d->value.size() + 1) * sizeof(wchar_t);
                auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (memory) {
                    auto target = GlobalLock(memory);
                    if (target) {
                        memcpy(target, d->value.c_str(), bytes);
                        GlobalUnlock(memory);
                        EmptyClipboard();
                        if (!SetClipboardData(CF_UNICODETEXT, memory))
                            GlobalFree(memory);
                    } else
                        GlobalFree(memory);
                }
                CloseClipboard();
            }
            return 0;
        }
        if (m == WM_PAINT || m == WM_PRINTCLIENT) {
            PAINTSTRUCT p{};
            auto target = m == WM_PAINT ? BeginPaint(h, &p) : reinterpret_cast<HDC>(w);
            {
                RECT r{};
                GetClientRect(h, &r);
                UiBufferedPaint buffer(target, r);
                auto dc = buffer.Dc();
                const auto c = UiFieldColors(*d->style);
                auto brush = CreateSolidBrush(c.surface);
                FillRect(dc, &r, brush);
                DeleteObject(brush);
                UiBox(dc, r, c.field, GetFocus() == h ? UiAccentInk(*d->style->dark) : c.border, D(*d, 10));
                const auto old = SelectObject(dc, d->style->font);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, IsWindowEnabled(h) ? c.text : c.muted);
                SIZE size{};
                GetTextExtentPoint32W(dc, d->value.c_str(), static_cast<int>(d->value.size()), &size);
                const int available = std::max(1L, r.right - D(*d, 16));
                d->maximum = std::max(0, static_cast<int>(size.cx) - available);
                d->offset = std::clamp(d->offset, 0, d->maximum);
                RECT text{D(*d, 8), D(*d, 4), r.right - D(*d, 8), r.bottom - D(*d, 10)};
                auto display = d->value;
                if (!d->expanded) {
                    while (display.size() > 1 && display.back() == L'/')
                        display.pop_back();
                    const auto slash = display.rfind(L'/');
                    if (slash != std::wstring::npos && slash + 1 < display.size())
                        display = display.substr(slash + 1);
                }
                const int saved = SaveDC(dc);
                IntersectClipRect(dc, text.left, text.top, text.right, text.bottom);
                if (d->expanded)
                    text.left -= d->offset;
                DrawTextW(dc, display.c_str(), -1, &text,
                          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (d->expanded ? 0 : DT_END_ELLIPSIS));
                RestoreDC(dc, saved);
                if (d->expanded && d->maximum) {
                    const auto t = Thumb(h, *d);
                    UiBox(dc, t, c.border, c.border, D(*d, 3));
                }
                SelectObject(dc, old);
            } // Commit the buffer while the BeginPaint DC is still valid.
            if (m == WM_PAINT)
                EndPaint(h, &p);
            return 0;
        }
        if (m == WM_LBUTTONDBLCLK || (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_F2))) {
            SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED),
                         reinterpret_cast<LPARAM>(h));
            return 0;
        }
        if (m == WM_LBUTTONDOWN) {
            SetFocus(h);
            RECT r{};
            GetClientRect(h, &r);
            const auto thumb = Thumb(h, *d);
            if (d->expanded && d->maximum && GET_Y_LPARAM(l) >= r.bottom - D(*d, 12)) {
                d->dragging = true;
                d->grab = PtInRect(&thumb, POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)}) ? GET_X_LPARAM(l) - thumb.left
                                                                                    : (thumb.right - thumb.left) / 2;
                SetCapture(h);
                SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, l);
            } else {
                d->expanded = true;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        if (m == WM_MOUSEMOVE && d->dragging) {
            RECT r{};
            GetClientRect(h, &r);
            const auto thumb = Thumb(h, *d);
            const int travel = std::max(1L, r.right - D(*d, 16) - (thumb.right - thumb.left));
            d->offset = MulDiv(std::clamp(GET_X_LPARAM(l) - D(*d, 8) - d->grab, 0, travel), d->maximum, travel);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (m == WM_LBUTTONUP || m == WM_CAPTURECHANGED || m == WM_CANCELMODE) {
            d->dragging = false;
            if (m != WM_CAPTURECHANGED && GetCapture() == h)
                ReleaseCapture();
        }
        if (m == WM_MOUSEHWHEEL ||
            (m == WM_KEYDOWN && (w == VK_LEFT || w == VK_RIGHT || w == VK_HOME || w == VK_END))) {
            d->expanded = true;
            if (m == WM_MOUSEHWHEEL)
                d->offset += GET_WHEEL_DELTA_WPARAM(w) * D(*d, 40) / WHEEL_DELTA;
            else if (w == VK_HOME)
                d->offset = 0;
            else if (w == VK_END)
                d->offset = d->maximum;
            else
                d->offset += w == VK_LEFT ? -D(*d, 40) : D(*d, 40);
            d->offset = std::clamp(d->offset, 0, d->maximum);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (m == WM_SETFOCUS || m == WM_KILLFOCUS)
            InvalidateRect(h, nullptr, FALSE);
        if (m == WM_NCDESTROY) {
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            delete d;
        }
        return DefWindowProcW(h, m, w, l);
    }

  public:
    static HWND Create(HWND owner, int id, int x, int y, int width, int height, UiFieldStyle *style) {
        WNDCLASSW c{};
        c.lpfnWndProc = Proc;
        c.hInstance = GetModuleHandleW(nullptr);
        c.lpszClassName = L"SerialCtlPathField";
        c.style = CS_DBLCLKS;
        c.hCursor = LoadCursorW(nullptr, IDC_HAND);
        RegisterClassW(&c);
        return CreateWindowExW(0, c.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, width, height, owner,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), c.hInstance, style);
    }
};
} // namespace serialctl
