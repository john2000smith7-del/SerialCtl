#include "AiPetPane.h"
#include "UiDialogViewport.h"
#include <cmath>
#include <windowsx.h>

namespace serialctl {
namespace {
enum Command { Toggle = 1, Idle, Wave, Boxing, Stop, Small, Medium, Large, Hide, Settings };
constexpr UINT_PTR PaintTimer = 1;
void Label(HDC dc, HFONT font, COLORREF ink, const std::wstring &text, RECT r,
           UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    const auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}
struct Popup {
    HWND window = nullptr, owner = nullptr;
    HFONT font = nullptr;
    pet::Settings prefs;
    bool dark = false, done = false, inSub = false;
    int dpi = 96, width = 220, row = 32, sub = 0, selected = 0, subSelected = 0, result = 0;
    POINT anchor{};
    RECT work{};
    int D(int n) const {
        return MulDiv(n, dpi, 96);
    }
    int MainHeight() const {
        return D(16 + 32 * 5 + 8);
    }
    int SubCount() const {
        return sub == 1 ? 4 : 3;
    }
    int SubTop() const {
        return D(8 + 32 * (sub == 1 ? 1 : 2));
    }
    int Height() const {
        return std::max(MainHeight(), sub ? SubTop() + D(16 + 32 * SubCount()) : 0);
    }
    std::wstring MainText(int i) const {
        const wchar_t *rows[] = {L"跟随 AI 状态", L"播放动作", L"宠物大小", L"收起宠物", L"宠物设置…"};
        return i == 0 && !prefs.enabled ? L"显示宠物" : rows[i];
    }
    std::wstring SubText(int i) const {
        const wchar_t *actions[] = {L"待机", L"挥手", L"前后跳步＋拳击", L"停止演示"};
        const wchar_t *sizes[] = {L"小（96）", L"中（120）", L"大（144）"};
        return sub == 1 ? actions[i] : sizes[i];
    }
    RECT Row(int column, int index) const {
        const int left = column ? width + D(12) : 0, top = column ? SubTop() : 0;
        const int y = top + D(8) + index * row + (!column && index == 4 ? D(8) : 0);
        return {left + D(4), y, left + width - D(4), y + row};
    }
    void Resize() {
        const int total = width + (sub ? D(12) + width : 0), height = Height();
        const int x =
            std::clamp(static_cast<int>(anchor.x), static_cast<int>(work.left) + D(12),
                       std::max(static_cast<int>(work.left) + D(12), static_cast<int>(work.right) - D(12) - total));
        const int y =
            std::clamp(static_cast<int>(anchor.y), static_cast<int>(work.top) + D(12),
                       std::max(static_cast<int>(work.top) + D(12), static_cast<int>(work.bottom) - D(12) - height));
        SetWindowPos(window, nullptr, x, y, total, height, SWP_NOZORDER | SWP_NOACTIVATE);
        HRGN region = CreateRoundRectRgn(0, 0, width, MainHeight(), D(24), D(24));
        if (sub) {
            HRGN other =
                CreateRoundRectRgn(width + D(12), SubTop(), total, SubTop() + D(16 + 32 * SubCount()), D(24), D(24));
            CombineRgn(region, region, other, RGN_OR);
            DeleteObject(other);
        }
        SetWindowRgn(window, region, TRUE);
        InvalidateRect(window, nullptr, FALSE);
    }
    void Close(int choice = 0) {
        if (done)
            return;
        done = true;
        result = choice;
        if (GetCapture() == window)
            ReleaseCapture();
        DestroyWindow(window);
        window = nullptr;
    }
    void Pick(bool child, int index) {
        if (child) {
            Close(sub == 1 ? static_cast<int>(Idle) + index : static_cast<int>(Small) + index);
            return;
        }
        if (index == 1 || index == 2) {
            sub = index;
            subSelected = 0;
            inSub = true;
            Resize();
            return;
        }
        Close(index == 0 ? Toggle : index == 3 ? Hide : Settings);
    }
    void Paint(HDC target) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        UiBufferedPaint paint(target, bounds);
        auto dc = paint.Dc();
        const auto c = UiTheme(dark);
        HBRUSH background = CreateSolidBrush(c.surface);
        FillRect(dc, &bounds, background);
        DeleteObject(background);
        UiBox(dc, {0, 0, width, MainHeight()}, c.surface, c.border, D(12));
        if (sub)
            UiBox(dc, {width + D(12), SubTop(), width * 2 + D(12), SubTop() + D(16 + 32 * SubCount())}, c.surface,
                  c.border, D(12));
        for (int column = 0; column <= (sub ? 1 : 0); ++column)
            for (int i = 0; i < (column ? SubCount() : 5); ++i) {
                const auto r = Row(column, i);
                const bool active = column ? inSub && i == subSelected : !inSub && i == selected;
                if (active)
                    UiBox(dc, r, c.accent, c.accent, D(8));
                const COLORREF ink = active ? RGB(255, 255, 255) : c.text;
                RECT text = r;
                text.left += D(36);
                text.right -= D(28);
                Label(dc, font, ink, column ? SubText(i) : MainText(i), text);
                RECT icon{r.left + D(10), r.top + (row - D(16)) / 2, r.left + D(26), r.top + (row + D(16)) / 2};
                if ((!column && i == 0 && prefs.enabled && prefs.follow) || (column && sub == 2 &&
                                                                             prefs.size == (i == 0   ? 96
                                                                                            : i == 1 ? 120
                                                                                                     : 144)))
                    UiDrawGlyph(dc, icon, ink, UiGlyph::Check);
                else if ((!column && i == 1) || (column && sub == 1 && i != 3))
                    UiDrawGlyph(dc, icon, ink, UiGlyph::Run);
                else if (column && i == 3)
                    UiDrawGlyph(dc, icon, ink, UiGlyph::Stop);
                else if (!column && i == 4)
                    UiDrawGlyph(dc, icon, ink, UiGlyph::Pet);
                if (!column && (i == 1 || i == 2))
                    UiDrawGlyph(dc, {r.right - D(22), icon.top, r.right - D(6), icon.bottom}, ink,
                                UiGlyph::ChevronRight);
            }
        UiSeparator(dc, D(12), width - D(12), D(8 + 32 * 4 + 4), c.border);
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto p = reinterpret_cast<Popup *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            p = static_cast<Popup *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            p->window = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
        }
        if (!p)
            return DefWindowProcW(h, m, w, l);
        if (m == WM_PAINT) {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(h, &ps);
            p->Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        if (m == WM_ERASEBKGND)
            return 1;
        if (m == WM_MOUSEMOVE || m == WM_LBUTTONUP) {
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            bool found = false;
            if (p->sub)
                for (int i = 0; i < p->SubCount(); ++i)
                    if (PtInRect(&p->RowCache(1, i), point)) {
                        p->inSub = true;
                        p->subSelected = i;
                        found = true;
                        if (m == WM_LBUTTONUP)
                            p->Pick(true, i);
                        break;
                    }
            if (!found)
                for (int i = 0; i < 5; ++i) {
                    const auto r = p->Row(0, i);
                    if (PtInRect(&r, point)) {
                        p->selected = i;
                        p->inSub = false;
                        found = true;
                        const int next = i == 1 || i == 2 ? i : 0;
                        if (next != p->sub) {
                            p->sub = next;
                            p->subSelected = 0;
                            p->Resize();
                        }
                        if (m == WM_LBUTTONUP)
                            p->Pick(false, i);
                        break;
                    }
                }
            if (m == WM_LBUTTONUP && !found)
                p->Close();
            if (p->window)
                InvalidateRect(p->window, nullptr, FALSE);
            return 0;
        }
        if (m == WM_KEYDOWN) {
            auto &selected = p->inSub ? p->subSelected : p->selected;
            const int count = p->inSub ? p->SubCount() : 5;
            if (w == VK_ESCAPE) {
                p->Close();
                return 0;
            }
            if (w == VK_UP || w == VK_DOWN)
                selected = (selected + count + (w == VK_UP ? -1 : 1)) % count;
            else if (w == VK_LEFT) {
                p->inSub = false;
                p->sub = 0;
                p->Resize();
            } else if (w == VK_RIGHT && !p->inSub && (p->selected == 1 || p->selected == 2)) {
                p->sub = p->selected;
                p->subSelected = 0;
                p->inSub = true;
                p->Resize();
            } else if (w == VK_RETURN || w == VK_SPACE)
                p->Pick(p->inSub, selected);
            if (p->window) {
                SetWindowTextW(p->window, (p->inSub ? p->SubText(p->subSelected) : p->MainText(p->selected)).c_str());
                InvalidateRect(p->window, nullptr, FALSE);
            }
            return 0;
        }
        if ((m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE && IsWindowVisible(h)) || m == WM_CLOSE ||
            m == WM_CANCELMODE || m == WM_RBUTTONUP) {
            p->Close();
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    RECT rowCache{};
    const RECT &RowCache(int column, int index) {
        rowCache = Row(column, index);
        return rowCache;
    }
    int Show() {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
        work = monitor.rcWork;
        width = std::min(D(220), std::max(D(96), (static_cast<int>(work.right - work.left) - D(36)) / 2));
        row = D(32);
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SerialCtlPetMenu";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        const HWND previous = GetFocus();
        window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"AI 宠物菜单", WS_POPUP, 0, 0, width,
                                 MainHeight(), owner, nullptr, wc.hInstance, this);
        if (!window)
            return 0;
        Resize();
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        SetFocus(window);
        SetCapture(window);
        MSG message{};
        while (!done) {
            const BOOL got = GetMessageW(&message, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0)
                    PostQuitMessage(static_cast<int>(message.wParam));
                Close();
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(owner)) {
            SetForegroundWindow(owner);
            if (IsWindow(previous) && IsWindowEnabled(previous))
                SetFocus(previous);
        }
        return result;
    }
};
struct PreferencesDialog {
    AiPetPane *self;
    pet::Settings draft;
    UiFieldStyle style{};
    HBRUSH surface = nullptr, field = nullptr;
};
} // namespace

AiPetPane::~AiPetPane() {
    Shutdown();
}
bool AiPetPane::Create(HWND parent, HFONT font, bool dark, pet::Settings settings) {
    parent_ = parent;
    font_ = font;
    dark_ = dark;
    HDC dc = GetDC(parent);
    dpi_ = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(parent, dc);
    LOGFONTW lf{};
    GetObjectW(font, sizeof(lf), &lf);
    lf.lfHeight = -D(12);
    small_ = CreateFontIndirectW(&lf);
    model_.Configure(settings, GetTickCount64());
    image_ = std::make_unique<UiResourceBitmap>(2013);
    if (!image_->bitmap || image_->bitmap->GetWidth() != 2048 || image_->bitmap->GetHeight() != 1280) {
        image_.reset();
        return false;
    }
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = L"SerialCtlAiPetPane";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    window_ = CreateWindowExW(0, wc.lpszClassName, L"AI 宠物助手", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                              parent, nullptr, wc.hInstance, this);
    return window_ != nullptr;
}
int AiPetPane::DesiredHeight() const {
    if (!image_ || !Preferences().enabled)
        return 0;
    const int logical = Preferences().size + (Preferences().size * 40 + 99) / 100 + 28 + 24;
    return D((logical + 7) / 8 * 8);
}
void AiPetPane::Present(bool aiPage, bool available) {
    const auto now = GetTickCount64();
    model_.Page(aiPage, now);
    available_ = aiPage && available && model_.Active();
    if (window_) {
        if (available_)
            SetTimer(window_, PaintTimer, 40, nullptr);
        else
            KillTimer(window_, PaintTimer);
        Tick();
    }
}
void AiPetPane::Theme(bool dark) {
    dark_ = dark;
    if (window_)
        InvalidateRect(window_, nullptr, FALSE);
}
void AiPetPane::Activity(pet::Activity state) {
    model_.SetActivity(state, GetTickCount64());
    if (window_ && available_)
        Tick();
}
void AiPetPane::Shutdown() {
    if (window_ && IsWindow(window_)) {
        KillTimer(window_, PaintTimer);
        DestroyWindow(window_);
    }
    window_ = nullptr;
    image_.reset();
    if (small_)
        DeleteObject(small_);
    small_ = nullptr;
}
void AiPetPane::Tick() {
    if (!window_)
        return;
    const bool present = available_ && IsWindowVisible(window_) && IsWindowEnabled(parent_) && !IsIconic(parent_);
    sample_ = model_.At(GetTickCount64(), present);
    if (present)
        InvalidateRect(window_, nullptr, FALSE);
    if (GetEnvironmentVariableW(L"SERIALCTL_UI_METRICS", nullptr, 0)) {
        SetPropW(window_, L"SerialCtl.PetPose", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(sample_.pose + 1)));
        SetPropW(window_, L"SerialCtl.PetAction",
                 reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(model_.CurrentAction()) + 1));
        SetPropW(window_, L"SerialCtl.PetHop", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(sample_.hop + 1)));
    }
}
void AiPetPane::Paint(HDC target) {
    RECT r{};
    GetClientRect(window_, &r);
    UiBufferedPaint paint(target, r);
    auto dc = paint.Dc();
    const auto c = UiTheme(dark_);
    auto brush = CreateSolidBrush(c.surface);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
    if (!image_ || !image_->bitmap)
        return;
    const int width = r.right, height = r.bottom;
    const double body = std::min<double>(D(Preferences().size), std::max(1, width - D(16)) * 208.0 / 248);
    const double scale = body / 208.0;
    const double travel = std::max(0.0, (width - 248 * scale) / 2 - D(4));
    const double anchorOffset = (Preferences().x - 500) / 500.0 * travel;
    const double motion = std::clamp(sample_.x * body, -travel, travel);
    const double x = width / 2.0 + std::clamp(anchorOffset + motion, -travel, travel);
    const double spare = std::max(0.0, height - D(28 + 16) - body * 1.40 - D(8));
    const double base = height - D(8) - spare * (1000 - Preferences().y) / 1000;
    const double lift = sample_.lift * body;
    const double side = 256 * scale;
    const double top = base - 240 * scale * sample_.breath - lift;
    sprite_ = {static_cast<LONG>(x - side / 2), static_cast<LONG>(top), static_cast<LONG>(x + side / 2),
               static_cast<LONG>(top + side)};
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush shadow(Gdiplus::Color(static_cast<BYTE>(dark_ ? 22 : 18), 80, 85, 100));
    const float shadowWidth = static_cast<float>(body * .65 * (1 - std::min(.5, sample_.lift)));
    graphics.FillEllipse(&shadow, static_cast<float>(x - shadowWidth / 2), static_cast<float>(base - D(2)), shadowWidth,
                         static_cast<float>(D(6)));
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    const unsigned pose = std::min(sample_.pose, 35u);
    graphics.DrawImage(image_->bitmap.get(),
                       Gdiplus::RectF(static_cast<float>(x - side / 2), static_cast<float>(top),
                                      static_cast<float>(side), static_cast<float>(side * sample_.breath)),
                       static_cast<float>((pose % 8) * 256), static_cast<float>((pose / 8) * 256), 256.f, 256.f,
                       Gdiplus::UnitPixel);
    if (!sample_.label.empty()) {
        const int bubbleTop = std::max(D(4), static_cast<int>(base - body - lift) - D(36));
        RECT bubble{D(4), bubbleTop, width - D(4), bubbleTop + D(28)};
        UiBox(dc, bubble, c.surface, c.border, D(10));
        InflateRect(&bubble, -D(8), 0);
        Label(dc, small_, c.text, sample_.label, bubble, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    menu_ = {width - D(28), D(40), width - D(4), D(64)};
    if (hover_ || GetFocus() == window_) {
        UiBox(dc, menu_, c.raised, c.border, D(10));
        const int cy = (menu_.top + menu_.bottom) / 2;
        for (int i = 0; i < 3; ++i) {
            RECT dot{menu_.left + D(7 + i * 4), cy - D(1), menu_.left + D(9 + i * 4), cy + D(1)};
            UiBox(dc, dot, c.text, c.text, D(1));
        }
    }
    if (GetFocus() == window_)
        UiFocusOutline(dc, {D(2), D(2), width - D(2), height - D(2)}, D(10), dpi_, UiFocusInk(dark_));
}
void AiPetPane::Changed() {
    if (window_)
        PostMessageW(parent_, AiPetPreferencesChanged, 0, 0);
}
void AiPetPane::ShowMenu(POINT point) {
    Popup popup;
    popup.owner = parent_;
    popup.font = font_;
    popup.dark = dark_;
    popup.dpi = dpi_;
    popup.anchor = point;
    popup.prefs = Preferences();
    const int choice = popup.Show();
    if (choice)
        Action(choice);
}
void AiPetPane::Action(int action) {
    auto settings = Preferences();
    const auto now = GetTickCount64();
    if (action == Settings) {
        SettingsDialog();
        return;
    }
    if (action == Toggle) {
        if (!settings.enabled)
            settings.enabled = true;
        else
            settings.follow = !settings.follow;
    } else if (action == Hide)
        settings.enabled = false;
    else if (action >= Small && action <= Large)
        settings.size = action == Small ? 96 : action == Medium ? 120 : 144;
    else if (action == Stop)
        model_.Stop(now);
    if (action >= Idle && action <= Boxing) {
        settings.enabled = true;
        model_.Configure(settings, now);
        model_.Play(action == Idle ? pet::Action::Idle : action == Wave ? pet::Action::Wave : pet::Action::Boxing, now);
    } else
        model_.Configure(settings, now);
    Changed();
    Tick();
}
LRESULT CALLBACK AiPetPane::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<AiPetPane *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<AiPetPane *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, m, w, l);
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        self->Paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_TIMER && w == PaintTimer) {
        self->Tick();
        return 0;
    }
    if (m == WM_SIZE || m == WM_SETFOCUS || m == WM_KILLFOCUS) {
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        if (!self->hover_) {
            self->hover_ = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
            TrackMouseEvent(&track);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (self->dragging_ && (w & MK_LBUTTON)) {
            const int dx = GET_X_LPARAM(l) - self->grab_.x, dy = GET_Y_LPARAM(l) - self->grab_.y;
            if (abs(dx) > GetSystemMetrics(SM_CXDRAG) || abs(dy) > GetSystemMetrics(SM_CYDRAG))
                self->moved_ = true;
            if (self->moved_) {
                auto p = self->dragStart_;
                RECT bounds{};
                GetClientRect(h, &bounds);
                const double body =
                    std::min<double>(self->D(p.size), std::max(1L, bounds.right - self->D(16)) * 208.0 / 248);
                const double travel = std::max(0.0, (bounds.right - 248 * body / 208) / 2 - self->D(4));
                const double spare = std::max(0.0, bounds.bottom - self->D(52) - body * 1.40);
                if (travel > 0)
                    p.x += static_cast<int>(std::lround(dx * 500.0 / travel));
                if (spare > 0)
                    p.y += static_cast<int>(std::lround(dy * 1000.0 / spare));
                self->model_.Configure(p, GetTickCount64());
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    }
    if (m == WM_MOUSELEAVE) {
        self->hover_ = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        SetFocus(h);
        if (PtInRect(&self->menu_, p)) {
            ClientToScreen(h, &p);
            self->ShowMenu(p);
            return 0;
        }
        if (PtInRect(&self->sprite_, p)) {
            self->dragging_ = true;
            self->moved_ = false;
            self->grab_ = p;
            self->dragStart_ = self->Preferences();
            SetCapture(h);
        }
        return 0;
    }
    if (m == WM_LBUTTONUP && self->dragging_) {
        const bool moved = self->moved_;
        self->dragging_ = false;
        if (GetCapture() == h)
            ReleaseCapture();
        if (moved)
            self->Changed();
        else
            self->Action(Wave);
        return 0;
    }
    if (m == WM_CAPTURECHANGED && self->dragging_) {
        self->dragging_ = false;
        self->model_.Configure(self->dragStart_, GetTickCount64());
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (m == WM_CONTEXTMENU) {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (p.x == -1) {
            p = {self->menu_.left, self->menu_.top};
            ClientToScreen(h, &p);
        }
        self->ShowMenu(p);
        return 0;
    }
    if (m == WM_GETDLGCODE)
        return DLGC_WANTARROWS;
    if (m == WM_KEYDOWN) {
        if (w == VK_APPS || (w == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000))) {
            POINT p{self->menu_.left, self->menu_.top};
            ClientToScreen(h, &p);
            self->ShowMenu(p);
        } else if (w == VK_SPACE || w == VK_RETURN)
            self->Action(Wave);
        else if (w == VK_ESCAPE) {
            if (self->dragging_) {
                self->model_.Configure(self->dragStart_, GetTickCount64());
                self->dragging_ = false;
                if (GetCapture() == h)
                    ReleaseCapture();
            }
            self->Action(Stop);
        } else if (w >= VK_LEFT && w <= VK_DOWN) {
            auto p = self->Preferences();
            if (w == VK_LEFT)
                p.x -= 50;
            if (w == VK_RIGHT)
                p.x += 50;
            if (w == VK_UP)
                p.y -= 50;
            if (w == VK_DOWN)
                p.y += 50;
            self->model_.Configure(p, GetTickCount64());
            self->Changed();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    if (m == WM_DESTROY) {
        KillTimer(h, PaintTimer);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void AiPetPane::SettingsDialog() {
    PreferencesDialog data{this, Preferences()};
    data.style = {&dark_, font_, dpi_};
    const auto c = UiTheme(dark_);
    data.surface = CreateSolidBrush(c.surface);
    data.field = CreateSolidBrush(c.field);
    struct Template {
        DLGTEMPLATE d{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 0, 0, 0, 0, 238, 180};
        WORD menu = 0, cls = 0, title = 0;
    } dialog;
    if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog.d, parent_, SettingsProc,
                                reinterpret_cast<LPARAM>(&data)) == IDOK) {
        model_.Configure(data.draft, GetTickCount64());
        Changed();
        Tick();
    }
    DeleteObject(data.surface);
    DeleteObject(data.field);
}
INT_PTR CALLBACK AiPetPane::SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto data = reinterpret_cast<PreferencesDialog *>(GetWindowLongPtrW(h, DWLP_USER));
    if (m == WM_INITDIALOG) {
        data = reinterpret_cast<PreferencesDialog *>(l);
        SetWindowLongPtrW(h, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        auto self = data->self;
        SetWindowTextW(h, L"AI 宠物助手");
        auto child = [&](const wchar_t *cls, const wchar_t *text, int id, DWORD style, int x, int y, int width,
                         int height) {
            HWND c = CreateWindowExW(
                0, cls, text, WS_CHILD | WS_VISIBLE | style, self->D(x), self->D(y), self->D(width), self->D(height), h,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(self->font_), FALSE);
            if (wcscmp(cls, L"BUTTON") == 0) {
                if ((style & BS_TYPEMASK) == BS_AUTOCHECKBOX)
                    UiStyleCheckBox(c, &data->style, true);
                else
                    UiStyleButton(c);
            }
            return c;
        };
        child(L"BUTTON", L"显示 AI 宠物助手", 101, BS_AUTOCHECKBOX | WS_TABSTOP, 16, 16, 328, 28);
        child(L"BUTTON", L"跟随 AI 的任务状态", 102, BS_AUTOCHECKBOX | WS_TABSTOP, 16, 52, 328, 28);
        child(L"BUTTON", L"打开 AI 后自动演示（4 秒后）", 103, BS_AUTOCHECKBOX | WS_TABSTOP, 16, 88, 328, 28);
        for (int id : {101, 102, 103})
            SendDlgItemMessageW(h, id, BM_SETCHECK,
                                (id == 101   ? data->draft.enabled
                                 : id == 102 ? data->draft.follow
                                             : data->draft.opening)
                                    ? BST_CHECKED
                                    : BST_UNCHECKED,
                                0);
        child(L"STATIC", L"宠物大小", -1, SS_LEFT, 16, 132, 328, 20);
        HWND combo = child(L"COMBOBOX", L"", 104, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP,
                           16, 156, 328, 180);
        UiStyleField(combo, &data->style);
        for (const wchar_t *text : {L"小（96）", L"中（120）", L"大（144）"})
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(combo, CB_SETCURSEL, data->draft.size == 96 ? 0 : data->draft.size == 120 ? 1 : 2, 0);
        child(L"BUTTON", L"恢复停靠位置", 105, BS_OWNERDRAW | WS_TABSTOP, 16, 196, 140, 28);
        child(L"BUTTON", L"取消", IDCANCEL, BS_OWNERDRAW | WS_TABSTOP, 140, 248, 96, 36);
        child(L"BUTTON", L"保存", IDOK, BS_OWNERDRAW | WS_TABSTOP, 248, 248, 96, 36);
        RECT r{0, 0, self->D(360), self->D(300)};
        AdjustWindowRectEx(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, FALSE, 0);
        RECT owner{};
        GetWindowRect(self->parent_, &owner);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(self->parent_, MONITOR_DEFAULTTONEAREST), &monitor);
        const int margin = self->D(12), width = r.right - r.left, height = r.bottom - r.top;
        const int x = std::clamp(static_cast<int>(owner.left + (owner.right - owner.left - width) / 2),
                                 static_cast<int>(monitor.rcWork.left) + margin,
                                 std::max(static_cast<int>(monitor.rcWork.left) + margin,
                                          static_cast<int>(monitor.rcWork.right) - width - margin));
        const int y = std::clamp(static_cast<int>(owner.top + (owner.bottom - owner.top - height) / 2),
                                 static_cast<int>(monitor.rcWork.top) + margin,
                                 std::max(static_cast<int>(monitor.rcWork.top) + margin,
                                          static_cast<int>(monitor.rcWork.bottom) - height - margin));
        SetWindowPos(h, nullptr, x, y, width, height, SWP_NOZORDER);
        UiDialogViewport::Fit(h, self->dpi_);
        return TRUE;
    }
    if (!data)
        return FALSE;
    auto self = data->self;
    const auto c = UiTheme(self->dark_);
    if (m == WM_COMMAND) {
        const int id = LOWORD(w);
        if (id == 105) {
            data->draft.x = data->draft.y = 500;
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        if (id == IDOK) {
            data->draft.enabled = IsDlgButtonChecked(h, 101) == BST_CHECKED;
            data->draft.follow = IsDlgButtonChecked(h, 102) == BST_CHECKED;
            data->draft.opening = IsDlgButtonChecked(h, 103) == BST_CHECKED;
            const auto selected = SendDlgItemMessageW(h, 104, CB_GETCURSEL, 0, 0);
            data->draft.size = selected == 1 ? 120 : selected == 2 ? 144 : 96;
            data->draft.Normalize();
            EndDialog(h, IDOK);
            return TRUE;
        }
    }
    if (m == WM_DRAWITEM) {
        auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        if (item.CtlType == ODT_COMBOBOX) {
            UiDrawComboItem(item, data->style);
            return TRUE;
        }
        UiBufferedPaint paint(item.hDC, item.rcItem);
        auto dc = paint.Dc();
        FillRect(dc, &item.rcItem, data->surface);
        const auto visual =
            UiButtonVisual(c, IsWindowEnabled(item.hwndItem) != FALSE, (item.itemState & ODS_SELECTED) != 0,
                           GetPropW(item.hwndItem, L"SerialCtl.PowerHover") != nullptr, item.CtlID == IDOK);
        UiBox(dc, item.rcItem, visual.fill, visual.border, self->D(10));
        wchar_t text[128]{};
        GetWindowTextW(item.hwndItem, text, 128);
        Label(dc, self->font_, visual.ink, text, item.rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        UiButtonKeyboardFocus(dc, item.rcItem, item.itemState, self->dpi_, UiFocusInk(self->dark_));
        return TRUE;
    }
    if (m == WM_MEASUREITEM) {
        auto &item = *reinterpret_cast<MEASUREITEMSTRUCT *>(l);
        if (item.CtlType == ODT_COMBOBOX) {
            item.itemHeight = self->D(28);
            return TRUE;
        }
    }
    if (m == WM_CTLCOLORDLG || m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN || m == WM_CTLCOLOREDIT ||
        m == WM_CTLCOLORLISTBOX) {
        auto dc = reinterpret_cast<HDC>(w);
        const bool field = m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c.text);
        SetBkColor(dc, field ? c.field : c.surface);
        return reinterpret_cast<INT_PTR>(field ? data->field : data->surface);
    }
    if (m == WM_CLOSE) {
        EndDialog(h, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
} // namespace serialctl
