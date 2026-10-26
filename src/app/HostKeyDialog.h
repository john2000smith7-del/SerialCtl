#pragma once
#include "PuttyHostKey.h"
#include "UiDialogViewport.h"
#include <functional>

namespace serialctl {

// Shared native confirmation for SSH and AI. No networking or trust mutation
// takes place on this UI thread. Fingerprints remain selectable/copyable text.
class HostKeyDialog {
public:
    static bool Show(HWND owner, const HostKeyChallenge& challenge, HFONT font,
                     bool dark, int dpi, std::function<bool()> cancelled = {}) {
        HostKeyDialog view(challenge, font, dark, dpi, std::move(cancelled));
        struct Template {
            DLGTEMPLATE dialog{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 0, 0, 0, 0, 300, 240};
            WORD menu = 0, cls = 0, title = 0;
        } resource;
        return UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &resource.dialog, owner,
                                       Proc, reinterpret_cast<LPARAM>(&view)) == IDOK;
    }
private:
    enum { Verified = 4100, OldFingerprint, NewFingerprint, Caption, Helper, Scope };
    HostKeyDialog(const HostKeyChallenge& challenge, HFONT font, bool dark, int dpi,
                  std::function<bool()> cancelled)
        : challenge_(challenge), font_(font), dark_(dark), dpi_(dpi),
          cancelled_(std::move(cancelled)), style_{&dark_, font_, dpi_} {
        surface_ = CreateSolidBrush(UiTheme(dark_).surface);
        raised_ = CreateSolidBrush(UiTheme(dark_).raised);
        code_ = CreateFontW(-D(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    }
    ~HostKeyDialog() {
        DeleteObject(surface_); DeleteObject(raised_);
        if (code_) DeleteObject(code_);
    }
    int D(int value) const { return MulDiv(value, dpi_, 96); }
    int TextHeight(HWND dialog, const std::wstring& text, HFONT font, int width) const {
        HDC dc = GetDC(dialog);
        auto old = SelectObject(dc, font);
        RECT rect{0, 0, D(width), 0};
        DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(dc, old); ReleaseDC(dialog, dc);
        return std::max(24, MulDiv(rect.bottom, 96, dpi_) + 4);
    }
    HWND Add(HWND dialog, const wchar_t* type, const std::wstring& text, int id,
             int x, int y, int width, int height, DWORD flags = 0, HFONT font = nullptr) {
        auto child = CreateWindowExW(0, type, text.c_str(), WS_CHILD | WS_VISIBLE | flags,
            D(x), D(y), D(width), D(height), dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : font_), TRUE);
        if (_wcsicmp(type, L"BUTTON") == 0) {
            if ((flags & BS_TYPEMASK) == BS_AUTOCHECKBOX) UiStyleCheckBox(child, &style_, true);
            else UiStyleButton(child);
        }
        return child;
    }
    int ReadOnlyText(HWND h, const std::wstring& text, int id, int x, int y, int width, HFONT font) {
        const auto edit = Add(h, L"EDIT", text, id, x, y, width, 24,
            ES_MULTILINE | ES_READONLY | WS_TABSTOP, font);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        HDC dc = GetDC(edit); auto previous = SelectObject(dc, font);
        TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
        SelectObject(dc, previous); ReleaseDC(edit, dc);
        const int lines = static_cast<int>(SendMessageW(edit, EM_GETLINECOUNT, 0, 0));
        const int height = std::max(24, MulDiv(std::max(1, lines) * metrics.tmHeight, 96, dpi_) + 4);
        SetWindowPos(edit, nullptr, 0, 0, D(width), D(height), SWP_NOMOVE | SWP_NOZORDER);
        SetWindowSubclass(edit, ReadOnlyProc, 9206, 0);
        return height;
    }
    static LRESULT CALLBACK ReadOnlyProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
        if (m == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, -1); return 0;
        }
        if (m == WM_NCDESTROY) RemoveWindowSubclass(h, ReadOnlyProc, id);
        return DefSubclassProc(h, m, w, l);
    }
    int FingerprintCard(HWND h, int y, const wchar_t* label, std::wstring text, int id) {
        if (text.empty()) text = L"PuTTY 已保存的主机身份（未保存可显示的指纹）";
        const auto digest = text.find(L" SHA256:");
        if (digest != std::wstring::npos) text.replace(digest, 1, L"\r\n");
        auto background = Add(h, L"STATIC", L"", id + 200, 16, y, width_ - 32, 76, SS_OWNERDRAW);
        Add(h, L"STATIC", label, id + 100, 32, y + 12, width_ - 64, 24);
        const int height = ReadOnlyText(h, text, id, 32, y + 40, width_ - 64, code_ ? code_ : font_);
        SetWindowPos(background, nullptr, 0, 0, D(width_ - 32), D(52 + height), SWP_NOMOVE | SWP_NOZORDER);
        return y + 52 + height;
    }
    void Initialize(HWND h) {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(GetWindow(h, GW_OWNER), MONITOR_DEFAULTTONEAREST), &monitor);
        RECT frame{};
        AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE)), FALSE,
                          static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE)));
        width_ = std::clamp(MulDiv(monitor.rcWork.right - monitor.rcWork.left - D(24) -
                                  (frame.right - frame.left), 96, dpi_), 280, 600);
        const int contentWidth = width_ - 32;
        const bool changed = challenge_.reason == HostKeyReason::Changed;
        const bool first = challenge_.reason == HostKeyReason::FirstConnection;
        SetWindowTextW(h, changed ? L"SSH 主机身份已变化" : first ? L"核验 SSH 主机身份" : L"重新核验 SSH 主机身份");
        int y = 16;
        const std::wstring introduction = changed ? L"服务器提供的指纹与已保存记录不一致。"
            : first ? L"首次连接此服务器，请核验主机身份。" : L"请核对当前服务器的主机身份。";
        const int introHeight = TextHeight(h, introduction, font_, contentWidth);
        Add(h, L"STATIC", introduction, Caption, 16, y, contentWidth, introHeight);
        y += introHeight + 8;
        const std::wstring helper = L"请向服务器管理员核对新指纹后再确认。";
        const int helperHeight = TextHeight(h, helper, font_, contentWidth);
        Add(h, L"STATIC", helper, Helper, 16, y, contentWidth, helperHeight);
        y += helperHeight + 16;
        const auto endpoint = L"服务器  " + challenge_.host + L":" + std::to_wstring(challenge_.port);
        const int endpointHeight = ReadOnlyText(h, endpoint, -1, 16, y, contentWidth, font_);
        y += endpointHeight + 12;
        if (!first) {
            y = FingerprintCard(h, y, L"已保存指纹", challenge_.previousFingerprint, OldFingerprint);
            y += 12;
        }
        y = FingerprintCard(h, y, first ? L"服务器指纹" : L"当前服务器指纹",
                            challenge_.fingerprint, NewFingerprint);
        y += 16;
        Add(h, L"BUTTON", L"我已通过可信渠道核对新指纹", Verified, 16, y, contentWidth, 28,
            BS_AUTOCHECKBOX | WS_TABSTOP);
        y += 28 + 8;
        const std::wstring scope = L"仅更新此服务器的身份记录，不重发 AI 指令。";
        const int scopeHeight = TextHeight(h, scope, font_, contentWidth);
        Add(h, L"STATIC", scope, Scope, 16, y, contentWidth, scopeHeight);
        y += scopeHeight + 24;
        const int buttonWidth = std::min(160, (contentWidth - 12) / 2);
        Add(h, L"BUTTON", L"取消", IDCANCEL, width_ - 16 - 2 * buttonWidth - 12, y, buttonWidth, 36, BS_OWNERDRAW | WS_TABSTOP);
        Add(h, L"BUTTON", first ? L"信任并连接" : changed ? (buttonWidth < 160 ? L"更新并重连" : L"更新并重新连接")
                                               : (buttonWidth < 160 ? L"确认并重连" : L"确认并重新连接"),
            IDOK, width_ - 16 - buttonWidth, y, buttonWidth, 36, BS_OWNERDRAW | WS_TABSTOP);
        EnableWindow(GetDlgItem(h, IDOK), FALSE);
        UiResizeDialog(h, dpi_, width_, y + 36 + 16);
        RECT bounds{}, owner{}; GetWindowRect(h, &bounds); GetWindowRect(GetWindow(h, GW_OWNER), &owner);
        SetWindowPos(h, nullptr, owner.left + ((owner.right - owner.left) - (bounds.right - bounds.left)) / 2,
            owner.top + ((owner.bottom - owner.top) - (bounds.bottom - bounds.top)) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER);
        // Consent and actions stay together on small work areas; only the
        // fingerprint/body region scrolls, so no partial checkbox can paint
        // across the footer or disappear while confirming.
        UiDialogViewport::Fit(h, dpi_, {Verified, Scope, IDCANCEL, IDOK});
        SetTimer(h, 1, 100, nullptr);
        SetFocus(GetDlgItem(h, IDCANCEL));
    }
    static INT_PTR CALLBACK Proc(HWND h, UINT message, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<HostKeyDialog*>(GetWindowLongPtrW(h, DWLP_USER));
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<HostKeyDialog*>(l); SetWindowLongPtrW(h, DWLP_USER, l);
            self->Initialize(h); return FALSE;
        }
        if (!self) return FALSE;
        if (message == WM_TIMER && self->cancelled_ && self->cancelled_()) {
            UiEndOwnedDialog(h, IDCANCEL); return TRUE;
        }
        if (message == WM_CLOSE) { UiEndOwnedDialog(h, IDCANCEL); return TRUE; }
        if (message == WM_COMMAND) {
            if (LOWORD(w) == Verified) {
                EnableWindow(GetDlgItem(h, IDOK), IsDlgButtonChecked(h, Verified) == BST_CHECKED);
                return TRUE;
            }
            if (LOWORD(w) == IDCANCEL) { UiEndOwnedDialog(h, IDCANCEL); return TRUE; }
            if (LOWORD(w) == IDOK) {
                if (IsDlgButtonChecked(h, Verified) == BST_CHECKED &&
                    (!self->cancelled_ || !self->cancelled_())) UiEndOwnedDialog(h, IDOK);
                return TRUE;
            }
        }
        const auto c = UiTheme(self->dark_);
        if (message == WM_ERASEBKGND) {
            RECT r{}; GetClientRect(h, &r); FillRect(reinterpret_cast<HDC>(w), &r, self->surface_); return TRUE;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; auto target = BeginPaint(h, &paint);
            RECT r{}; GetClientRect(h, &r);
            { UiBufferedPaint buffer(target, r); FillRect(buffer.Dc(), &r, self->surface_); }
            EndPaint(h, &paint); return TRUE;
        }
        if (message == WM_DRAWITEM) {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (item.CtlType == ODT_STATIC) {
                // Owner-draw DCs can omit a partially clipped child region.
                // Clip the final blit explicitly before the fixed consent area.
                RECT card{}, consent{};
                GetWindowRect(item.hwndItem, &card); GetWindowRect(GetDlgItem(h, Verified), &consent);
                MapWindowPoints(HWND_DESKTOP, h, reinterpret_cast<POINT*>(&card), 2);
                MapWindowPoints(HWND_DESKTOP, h, reinterpret_cast<POINT*>(&consent), 2);
                const int saved = SaveDC(item.hDC);
                IntersectClipRect(item.hDC, item.rcItem.left, std::max<LONG>(item.rcItem.top, -card.top),
                    item.rcItem.right, std::max<LONG>(item.rcItem.top,
                        std::min<LONG>(item.rcItem.bottom, consent.top - self->D(12) - card.top)));
                { UiBufferedPaint buffer(item.hDC, item.rcItem);
                  FillRect(buffer.Dc(), &item.rcItem, self->surface_);
                  UiBox(buffer.Dc(), item.rcItem, c.raised, c.border, self->D(12)); }
                RestoreDC(item.hDC, saved);
                return TRUE;
            }
            UiBufferedPaint buffer(item.hDC, item.rcItem); auto dc = buffer.Dc();
            const bool enabled = !(item.itemState & ODS_DISABLED), primary = item.CtlID == IDOK;
            const auto visual = UiButtonVisual(c, enabled, (item.itemState & ODS_SELECTED) != 0,
                GetPropW(item.hwndItem, L"SerialCtl.PowerHover") != nullptr, primary);
            FillRect(dc, &item.rcItem, self->surface_);
            UiBox(dc, item.rcItem, visual.fill, visual.border, self->D(10));
            wchar_t text[64]{}; GetWindowTextW(item.hwndItem, text, 64);
            auto old = SelectObject(dc, self->font_); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, visual.ink);
            RECT r = item.rcItem; DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, old);
            UiButtonKeyboardFocus(dc, item.rcItem, item.itemState, self->dpi_, UiFocusInk(self->dark_, primary && enabled));
            return TRUE;
        }
        if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORBTN) {
            auto dc = reinterpret_cast<HDC>(w); const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            const bool card = id == OldFingerprint || id == NewFingerprint || id == OldFingerprint + 100 || id == NewFingerprint + 100;
            SetBkColor(dc, card ? c.raised : c.surface);
            SetTextColor(dc, id == Helper || id == Scope ? c.muted :
                (id == OldFingerprint + 100 || id == NewFingerprint + 100) ? UiAccentInk(self->dark_) : c.text);
            return reinterpret_cast<INT_PTR>(card ? self->raised_ : self->surface_);
        }
        return FALSE;
    }
    const HostKeyChallenge& challenge_;
    HFONT font_, code_ = nullptr;
    bool dark_;
    int dpi_, width_ = 600;
    std::function<bool()> cancelled_;
    UiFieldStyle style_;
    HBRUSH surface_ = nullptr, raised_ = nullptr;
};
} // namespace serialctl
