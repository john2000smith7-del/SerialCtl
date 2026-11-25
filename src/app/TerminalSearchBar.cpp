#include "TerminalSearchBar.h"
#include "UiStyle.h"
#include "UiMetrics.h"
#include <algorithm>
#include <commctrl.h>

namespace serialctl {
namespace {

constexpr wchar_t kSearchBarClass[] = L"SerialCtlTerminalSearchBar";

void RegisterSearchBarClass(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = TerminalSearchBar::WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kSearchBarClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

} // namespace

TerminalSearchBar::~TerminalSearchBar() {
    Destroy();
}

bool TerminalSearchBar::Create(HWND parent, HINSTANCE instance, Callbacks callbacks) {
    if (window_) return true;
    if (!parent) return false;
    parent_ = parent;
    instance_ = instance ? instance : GetModuleHandleW(nullptr);
    callbacks_ = std::move(callbacks);
    RegisterSearchBarClass(instance_);

    window_ = CreateWindowExW(
        0, kSearchBarClass, L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 100, 36,
        parent_, nullptr, instance_, this);
    if (!window_) return false;

    edit_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 100, 24,
        window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdEdit)), instance_, nullptr);
    if (edit_) {
        SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索…"));
        SetWindowSubclass(edit_, EditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    auto makeButton = [&](int id, const wchar_t* label, UiGlyph glyph, const wchar_t* tip) -> HWND {
        HWND btn = CreateWindowExW(
            0, L"BUTTON", label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 28, 28,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (btn) {
            UiStyleButton(btn);
            UiSetFlatIconButton(btn);
            UiSetButtonGlyph(btn, glyph, tip);
        }
        return btn;
    };

    prevButton_ = makeButton(IdPrev, L"上一处", UiGlyph::ChevronUp, L"上一处匹配 (Shift+Enter / ↑)");
    nextButton_ = makeButton(IdNext, L"下一处", UiGlyph::ChevronDown, L"下一处匹配 (Enter / ↓)");
    closeButton_ = makeButton(IdClose, L"关闭", UiGlyph::Close, L"关闭搜索 (Esc)");

    UpdateFieldBrush();
    SetCountState(-1, 0, false);
    Layout();
    return true;
}

void TerminalSearchBar::Destroy() {
    if (edit_) {
        RemoveWindowSubclass(edit_, EditSubclassProc, 1);
        edit_ = nullptr;
    }
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    prevButton_ = nullptr;
    nextButton_ = nullptr;
    closeButton_ = nullptr;
    if (fieldBrush_) {
        DeleteObject(fieldBrush_);
        fieldBrush_ = nullptr;
    }
}

bool TerminalSearchBar::ContainsFocus() const {
    if (!window_) return false;
    HWND focused = GetFocus();
    return focused == window_ || (focused && IsChild(window_, focused) != FALSE);
}

void TerminalSearchBar::Open(bool selectAll) {
    if (!window_) return;
    Layout();
    ShowWindow(window_, SW_SHOW);
    SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (edit_) {
        SetFocus(edit_);
        if (selectAll) {
            SendMessageW(edit_, EM_SETSEL, 0, -1);
        }
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void TerminalSearchBar::Close(bool returnFocusToParent) {
    if (!window_) return;
    const bool hadFocus = ContainsFocus();
    hasAvoidRect_ = false;
    avoidRect_ = {};
    ShowWindow(window_, SW_HIDE);
    if (returnFocusToParent && hadFocus && parent_ && IsWindowVisible(parent_)) {
        SetFocus(parent_);
    }
}

void TerminalSearchBar::SetAvoidRect(const RECT* rect) {
    if (rect && rect->right > rect->left && rect->bottom > rect->top) {
        if (hasAvoidRect_ && EqualRect(&avoidRect_, rect)) return;
        hasAvoidRect_ = true;
        avoidRect_ = *rect;
    } else {
        if (!hasAvoidRect_) return;
        hasAvoidRect_ = false;
        avoidRect_ = {};
    }
    Layout();
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void TerminalSearchBar::Theme(bool dark, int dpi, HFONT font) {
    dark_ = dark;
    dpi_ = dpi > 0 ? dpi : 96;
    font_ = font;
    UpdateFieldBrush();
    if (edit_) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    for (HWND btn : {prevButton_, nextButton_, closeButton_}) {
        if (btn) {
            SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            InvalidateRect(btn, nullptr, FALSE);
        }
    }
    if (window_) {
        Layout();
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void TerminalSearchBar::UpdateFieldBrush() {
    if (fieldBrush_) {
        DeleteObject(fieldBrush_);
        fieldBrush_ = nullptr;
    }
    const auto colors = UiTheme(dark_);
    fieldBrush_ = CreateSolidBrush(colors.field);
}

std::wstring TerminalSearchBar::Query() const {
    if (!edit_) return {};
    const int length = GetWindowTextLengthW(edit_);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(edit_, text.data(), static_cast<int>(text.size()));
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

void TerminalSearchBar::SetCountState(int currentIndex, size_t totalMatches, bool hasQuery) {
    const bool navEnabled = hasQuery && totalMatches > 0;
    if (!hasQuery) {
        countText_.clear();
    } else if (totalMatches == 0) {
        countText_ = L"无匹配";
    } else {
        const int displayIdx = (currentIndex >= 0 && static_cast<size_t>(currentIndex) < totalMatches)
            ? (currentIndex + 1) : 1;
        countText_ = std::to_wstring(displayIdx) + L" / " + std::to_wstring(totalMatches);
    }
    if (prevButton_) EnableWindow(prevButton_, navEnabled ? TRUE : FALSE);
    if (nextButton_) EnableWindow(nextButton_, navEnabled ? TRUE : FALSE);
    if (window_) {
        Layout();
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void TerminalSearchBar::Layout() {
    if (!window_ || !parent_) return;
    RECT parentClient{};
    GetClientRect(parent_, &parentClient);
    const int parentW = static_cast<int>(parentClient.right - parentClient.left);
    const int parentH = static_cast<int>(parentClient.bottom - parentClient.top);
    if (parentW <= 0 || parentH <= 0) return;

    auto s = [this](int v) { return MulDiv(v, dpi_, 96); };
    const int gap = s(6);
    const int rightMargin = MulDiv(Ui::OverlayScrollLaneWidth96, dpi_, 96) + gap;
    const int leftMargin = gap;
    const int topMargin = gap;
    const int maxCardW = (std::max)(s(110), parentW - leftMargin - rightMargin);

    const int padX = s(6);
    const int padY = s(4);
    const int itemGap = s(4);
    const int btnGap = s(2);
    const int btnSize = s(Ui::CompactIconButtonSize);
    const int cardH = padY * 2 + btnSize;
    const int buttonsW = btnSize * 3 + btnGap * 2;

    int countW = 0;
    if (!countText_.empty()) {
        HDC dc = GetDC(window_);
        HFONT oldFont = dc && font_ ? reinterpret_cast<HFONT>(SelectObject(dc, font_)) : nullptr;
        RECT calc{0, 0, 0, 0};
        if (dc) {
            DrawTextW(dc, countText_.c_str(), static_cast<int>(countText_.size()), &calc, DT_CALCRECT | DT_SINGLELINE);
        }
        if (oldFont) SelectObject(dc, oldFont);
        if (dc) ReleaseDC(window_, dc);
        countW = (std::max)(s(44), static_cast<int>(calc.right - calc.left) + s(4));
    } else {
        countW = s(36);
    }

    const int idealEditW = s(170);
    const int minEditW = s(48);
    const int fixedWithoutEdit = padX * 2 + itemGap + countW + itemGap + buttonsW;
    int editW = idealEditW;
    if (fixedWithoutEdit + editW > maxCardW) {
        editW = (std::max)(minEditW, maxCardW - fixedWithoutEdit);
        if (fixedWithoutEdit + editW > maxCardW) {
            const int overflow = fixedWithoutEdit + editW - maxCardW;
            const int trimCount = (std::min)(overflow, (std::max)(0, countW - s(28)));
            countW -= trimCount;
            editW = (std::max)(s(24), maxCardW - (padX * 2 + itemGap + countW + itemGap + buttonsW));
        }
    }

    const int cardW = (std::min)(maxCardW, padX * 2 + editW + itemGap + countW + itemGap + buttonsW);
    const int defaultLeft = (std::max)(leftMargin, parentW - rightMargin - cardW);
    const int topY = (std::min)(topMargin, (std::max)(0, parentH - cardH));
    defaultCardBounds_ = {defaultLeft, topY, defaultLeft + cardW, topY + cardH};

    int cardLeft = defaultLeft;
    int cardTop = topY;
    if (hasAvoidRect_) {
        auto intersectsAvoid = [&](const RECT& r) -> bool {
            return r.left < avoidRect_.right && r.right > avoidRect_.left &&
                   r.top < avoidRect_.bottom && r.bottom > avoidRect_.top;
        };
        if (intersectsAvoid(defaultCardBounds_)) {
            const int bottomMargin = gap;
            const int bottomY = (std::max)(topMargin, parentH - bottomMargin - cardH);
            const int belowY = avoidRect_.bottom + gap;
            const int aboveY = avoidRect_.top - gap - cardH;
            RECT candidates[4]{
                {defaultLeft, belowY, defaultLeft + cardW, belowY + cardH},
                {defaultLeft, bottomY, defaultLeft + cardW, bottomY + cardH},
                {leftMargin, topY, leftMargin + cardW, topY + cardH},
                {defaultLeft, aboveY, defaultLeft + cardW, aboveY + cardH}
            };
            bool placed = false;
            for (int i = 0; i < 4; ++i) {
                const RECT& c = candidates[i];
                if (c.top >= topMargin && c.bottom <= parentH - bottomMargin &&
                    c.left >= leftMargin && c.right <= parentW - gap &&
                    !intersectsAvoid(c)) {
                    cardLeft = c.left;
                    cardTop = c.top;
                    placed = true;
                    break;
                }
            }
            if (!placed && bottomY >= topMargin && !intersectsAvoid(candidates[1])) {
                cardLeft = candidates[1].left;
                cardTop = candidates[1].top;
            }
        }
    }

    cardBounds_ = {cardLeft, cardTop, cardLeft + cardW, cardTop + cardH};
    MoveWindow(window_, cardLeft, cardTop, cardW, cardH, TRUE);

    int x = padX;
    editFrameRect_ = {x, padY, x + editW, padY + btnSize};
    if (edit_) {
        const int insetX = s(6);
        const int innerW = (std::max)(4, static_cast<int>(editFrameRect_.right - editFrameRect_.left) - insetX * 2);
        const int textH = UiEditTextHeight(edit_, (std::max)(1, btnSize - s(6)));
        const int editY = padY + (std::max)(0, (btnSize - textH) / 2);
        MoveWindow(edit_, editFrameRect_.left + insetX, editY, innerW, textH, FALSE);
    }
    x = editFrameRect_.right + itemGap;

    countRect_ = {x, padY, x + countW, padY + btnSize};
    x = countRect_.right + itemGap;

    if (prevButton_) {
        MoveWindow(prevButton_, x, padY, btnSize, btnSize, FALSE);
        x += btnSize + btnGap;
    }
    if (nextButton_) {
        MoveWindow(nextButton_, x, padY, btnSize, btnSize, FALSE);
        x += btnSize + btnGap;
    }
    if (closeButton_) {
        MoveWindow(closeButton_, x, padY, btnSize, btnSize, FALSE);
    }
}

LRESULT CALLBACK TerminalSearchBar::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    TerminalSearchBar* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TerminalSearchBar*>(cs->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    } else {
        self = reinterpret_cast<TerminalSearchBar*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TerminalSearchBar::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window_, &ps);
        Paint(dc);
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const auto colors = UiTheme(dark_);
        SetTextColor(dc, colors.text);
        SetBkColor(dc, colors.field);
        return reinterpret_cast<LRESULT>(fieldBrush_ ? fieldBrush_ : GetStockObject(NULL_BRUSH));
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item && item->CtlType == ODT_BUTTON) {
            DrawOwnerButton(*item);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == IdEdit && code == EN_CHANGE && !suppressChange_) {
            if (callbacks_.onQueryChanged) callbacks_.onQueryChanged(Query());
            return 0;
        }
        if (code == BN_CLICKED) {
            if (id == IdPrev) {
                if (callbacks_.onNavigate) callbacks_.onNavigate(-1);
                return 0;
            }
            if (id == IdNext) {
                if (callbacks_.onNavigate) callbacks_.onNavigate(1);
                return 0;
            }
            if (id == IdClose) {
                if (callbacks_.onClose) callbacks_.onClose();
                return 0;
            }
        }
        break;
    }
    case WM_LBUTTONDOWN:
        if (edit_) SetFocus(edit_);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        window_ = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

void TerminalSearchBar::Paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    if (w <= 0 || h <= 0) return;

    HDC buffer = paintBuffer_.Get(dc, w, h, dpi_);
    HDC target = buffer ? buffer : dc;
    const auto colors = UiTheme(dark_);

    HBRUSH bgBrush = CreateSolidBrush(colors.field);
    FillRect(target, &client, bgBrush);
    DeleteObject(bgBrush);

    UiBox(target, client, colors.surface, colors.border, MulDiv(12, dpi_, 96));

    const bool editFocused = edit_ && GetFocus() == edit_;
    const COLORREF editBorder = editFocused ? UiAccentInk(dark_) : colors.border;
    UiBox(target, editFrameRect_, colors.field, editBorder, MulDiv(6, dpi_, 96));

    if (!countText_.empty() && countRect_.right > countRect_.left) {
        HFONT oldFont = font_ ? reinterpret_cast<HFONT>(SelectObject(target, font_)) : nullptr;
        SetBkMode(target, TRANSPARENT);
        SetTextColor(target, colors.muted);
        RECT textRect = countRect_;
        DrawTextW(target, countText_.c_str(), static_cast<int>(countText_.size()),
                  &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (oldFont) SelectObject(target, oldFont);
    }

    if (buffer) {
        BitBlt(dc, 0, 0, w, h, buffer, 0, 0, SRCCOPY);
    }
}

void TerminalSearchBar::DrawOwnerButton(const DRAWITEMSTRUCT& item) {
    HDC dc = item.hDC;
    const RECT rect = item.rcItem;
    const auto colors = UiTheme(dark_);
    const bool enabled = (item.itemState & ODS_DISABLED) == 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hovered = GetPropW(item.hwndItem, Ui::HoverProperty) != nullptr;
    const auto v = UiFlatIconVisual(colors, colors.surface, enabled, pressed, hovered);

    HBRUSH bgBrush = CreateSolidBrush(colors.surface);
    FillRect(dc, &rect, bgBrush);
    DeleteObject(bgBrush);

    if (v.fill != colors.surface) {
        UiRoundedSurface(dc, rect, v.fill, v.fill, UiCapsuleRadius(rect), dpi_);
    }
    UiDrawButtonGlyph(dc, item, v.ink, dpi_);
    UiButtonKeyboardFocus(dc, rect, item.itemState, dpi_, v.ink);
}

LRESULT CALLBACK TerminalSearchBar::EditSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                                     UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<TerminalSearchBar*>(referenceData);
    if (self) {
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
            if (self->window_) InvalidateRect(self->window_, nullptr, FALSE);
        } else if (message == WM_KEYDOWN) {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            if (wParam == VK_ESCAPE) {
                if (self->callbacks_.onClose) self->callbacks_.onClose();
                return 0;
            }
            if (wParam == VK_RETURN) {
                if (self->callbacks_.onNavigate) self->callbacks_.onNavigate(shift ? -1 : 1);
                return 0;
            }
            if (wParam == VK_UP) {
                if (self->callbacks_.onNavigate) self->callbacks_.onNavigate(-1);
                return 0;
            }
            if (wParam == VK_DOWN) {
                if (self->callbacks_.onNavigate) self->callbacks_.onNavigate(1);
                return 0;
            }
            if (ctrl && !alt && (wParam == L'F' || wParam == L'f')) {
                SendMessageW(window, EM_SETSEL, 0, -1);
                return 0;
            }
        } else if (message == WM_CHAR) {
            if (wParam == L'\r' || wParam == L'\n' || wParam == 0x1B || wParam == 0x06) {
                return 0;
            }
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, EditSubclassProc, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace serialctl
