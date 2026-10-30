#include "CommandNotebook.h"
#include "UiStyle.h"
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
namespace serialctl {
namespace {
constexpr int EditorId = 410, MoreId = 411, FirstButton = 412;
void TextAt(HDC dc, HFONT font, RECT r, const std::wstring &s, COLORREF color,
            UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format);
    SelectObject(dc, old);
}
void Fill(HDC dc, RECT r, COLORREF color) {
    auto b = CreateSolidBrush(color);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
} // namespace
bool CommandNotebook::Create(HWND parent, int id) {
    static const auto rich = [] {
        wchar_t path[MAX_PATH]{};
        GetSystemDirectoryW(path, MAX_PATH);
        std::wstring dll = std::wstring(path) + L"\\msftedit.dll";
        return LoadLibraryW(dll.c_str());
    }();
    if (!rich)
        return false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SerialCtlCommandNotebook";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    window_ = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                              parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, this);
    return window_ && editor_;
}
LRESULT CALLBACK CommandNotebook::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<CommandNotebook *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<CommandNotebook *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->Message(m, w, l) : DefWindowProcW(h, m, w, l);
}
void CommandNotebook::Style(bool dark, int dpi, HFONT ui, HFONT mono) {
    dark_ = dark;
    dpi_ = dpi;
    ui_ = ui;
    mono_ = mono;
    gutter_ = D(48);
    for (auto b : buttons_)
        SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(ui), FALSE);
    SendMessageW(editor_, WM_SETFONT, reinterpret_cast<WPARAM>(mono), FALSE);
    const auto c = UiTheme(dark);
    SendMessageW(editor_, EM_SETBKGNDCOLOR, 0, c.field);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = c.text;
    SendMessageW(editor_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
    HDC dc = GetDC(editor_);
    auto old = SelectObject(dc, mono);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    rowHeight_ = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(editor_, dc);
    Layout();
    Refresh(true);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void CommandNotebook::SetText(const std::wstring &text, bool dirty) {
    loading_ = true;
    SetWindowTextW(editor_, text.c_str());
    SendMessageW(editor_, EM_EMPTYUNDOBUFFER, 0, 0);
    loading_ = false;
    Refresh(true);
    dirty_ = dirty;
    Selection(0, 0);
    InvalidateRect(window_, nullptr, FALSE);
}
void CommandNotebook::Selection(LONG a, LONG b) {
    CHARRANGE range{a, b};
    SendMessageW(editor_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    Refresh();
}
std::vector<std::wstring> CommandNotebook::SelectedCommands() const {
    return CommandSelection(text_, static_cast<size_t>(std::max(0L, selection_.cpMin)),
                            static_cast<size_t>(std::max(0L, selection_.cpMax)));
}
void CommandNotebook::State(bool writable, bool running, size_t sent, size_t total, std::wstring target) {
    const bool different =
        writable != writable_ || running != running_ || sent != sent_ || total != total_ || target != target_;
    const bool wasRunning = running_;
    writable_ = writable;
    running_ = running;
    sent_ = sent;
    total_ = total;
    target_ = std::move(target);
    EnableWindow(buttons_[0], running || writable);
    for (int i = 1; i < 4; ++i)
        EnableWindow(buttons_[i], !running);
    if (wasRunning != running)
        SendMessageW(editor_, EM_SETREADONLY, running, 0);
    if (different) {
        InvalidateRect(window_, nullptr, FALSE);
        for (auto b : buttons_)
            InvalidateRect(b, nullptr, FALSE);
    }
}
void CommandNotebook::Refresh(bool edit) {
    if (edit) {
        GETTEXTLENGTHEX len{GTL_NUMCHARS | GTL_PRECISE, 1200};
        const auto n = SendMessageW(editor_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&len), 0);
        std::wstring value(static_cast<size_t>(std::max<LRESULT>(0, n)) + 1, L'\0');
        GETTEXTEX get{static_cast<DWORD>(value.size() * sizeof(wchar_t)), GT_RAWTEXT, 1200, nullptr, nullptr};
        const auto count =
            SendMessageW(editor_, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get), reinterpret_cast<LPARAM>(value.data()));
        value.resize(static_cast<size_t>(std::max<LRESULT>(0, count)));
        if (value != text_) {
            text_ = std::move(value);
            lines_ = CommandLines(text_);
            if (!loading_)
                dirty_ = true;
        }
    }
    if (edit && mono_) {
        HDC dc = GetDC(editor_);
        const auto old = SelectObject(dc, mono_);
        contentWidth_ = 0;
        for (const auto &line : lines_) {
            SIZE size{};
            if (GetTextExtentPoint32W(dc, text_.data() + line.start, static_cast<int>(line.end - line.start), &size))
                contentWidth_ = std::max(contentWidth_, static_cast<int>(size.cx) + D(16));
        }
        SelectObject(dc, old);
        ReleaseDC(editor_, dc);
    }
    SendMessageW(editor_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection_));
    RECT client{};
    GetClientRect(window_, &client);
    RECT left{0, top_, gutter_, bottom_};
    RECT footer{0, bottom_, client.right, client.bottom};
    RECT right{client.right - D(16), top_, client.right, bottom_};
    InvalidateRect(window_, &left, FALSE);
    InvalidateRect(window_, &footer, FALSE);
    InvalidateRect(window_, &right, FALSE);
    if (edit) {
        RECT title{0, 0, client.right, D(32)};
        InvalidateRect(window_, &title, FALSE);
    }
    if (changed && !loading_)
        changed();
}
void CommandNotebook::Layout() {
    if (!editor_)
        return;
    RECT r{};
    GetClientRect(window_, &r);
    const int w = r.right;
    const int height = D(32), gap = D(8), buttonWidth = std::max(1, (w - gap * 3) / 4);
    MoveWindow(more_, std::max(0, w - D(28)), D(2), D(28), D(28), TRUE);
    for (int i = 0; i < 4; ++i)
        MoveWindow(buttons_[i], i * (buttonWidth + gap), D(40),
                   i == 3 ? std::max(1, w - i * (buttonWidth + gap)) : buttonWidth, height, TRUE);
    top_ = D(80);
    bottom_ = std::max(top_ + D(24), static_cast<int>(r.bottom) - D(48));
    MoveWindow(editor_, gutter_, top_, std::max(1, w - gutter_ - D(16)), std::max(1, bottom_ - top_ - D(16)), TRUE);
    RECT format{};
    GetClientRect(editor_, &format);
    format.left = D(4);
    format.top = 0;
    format.right = std::max<LONG>(format.left + 1, format.right - D(4));
    SendMessageW(editor_, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&format));
    InvalidateRect(window_, nullptr, FALSE);
}
void CommandNotebook::Paint(HDC dc) {
    RECT r{};
    GetClientRect(window_, &r);
    const auto c = UiTheme(dark_);
    Fill(dc, r, c.surface);
    RECT file{0, 0, r.right - D(36), D(32)};
    UiRoundedSurface(dc, {0, 0, r.right, D(32)}, c.surface, c.border, D(10), dpi_);
    UiDrawGlyph(dc, {D(8), D(8), D(24), D(24)}, c.muted, UiGlyph::Document);
    file.left = D(32);
    TextAt(dc, ui_, file, name_ + (dirty_ ? L"  •" : L""), c.text);
    RECT editRect{0, top_, r.right, bottom_};
    Fill(dc, editRect, c.field);
    POINT scroll{};
    SendMessageW(editor_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    const auto range = CommandSelectedLines(lines_, std::max(0L, selection_.cpMin), std::max(0L, selection_.cpMax));
    for (size_t line = static_cast<size_t>(std::max<LONG>(0, scroll.y / std::max(1, rowHeight_))); line < lines_.size();
         ++line) {
        POINTL point{};
        SendMessageW(editor_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&point),
                     static_cast<LPARAM>(lines_[line].start));
        const int y = top_ + point.y;
        if (y >= bottom_ - D(16))
            break;
        if (y + rowHeight_ <= top_)
            continue;
        RECT nr{D(18), y, gutter_ - D(6), y + rowHeight_};
        TextAt(dc, mono_, nr, std::to_wstring(line + 1), c.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (line >= range.first && line <= range.second && !CommandLineText(text_, lines_[line]).empty())
            UiDrawGlyph(dc, {D(1), y + (rowHeight_ - D(12)) / 2, D(13), y + (rowHeight_ + D(12)) / 2},
                        writable_ ? UiAccentInk(dark_) : c.muted, UiGlyph::Run);
    }
    vertical_ = {};
    horizontal_ = {};
    RECT er{};
    GetClientRect(editor_, &er);
    const int fullHeight = static_cast<int>(lines_.size()) * rowHeight_, viewHeight = er.bottom;
    if (fullHeight > viewHeight && viewHeight > 0) {
        const int track = bottom_ - top_ - D(16), thumb = std::max(D(28), MulDiv(track, viewHeight, fullHeight));
        const int y = top_ + MulDiv(std::clamp<int>(scroll.y, 0, std::max(1, fullHeight - viewHeight)), track - thumb,
                                    std::max(1, fullHeight - viewHeight));
        vertical_ = {r.right - D(8), y, r.right - D(3), y + thumb};
        UiRoundedSurface(dc, vertical_, c.border, c.border, D(3), dpi_);
    }
    // RichEdit keeps native horizontal ranges even with system chrome omitted.
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMax = contentWidth_ - 1;
    info.nPage = static_cast<UINT>(er.right);
    const int fullWidth = contentWidth_;
    if (fullWidth > static_cast<int>(info.nPage) && info.nPage) {
        const int track = std::max(1, static_cast<int>(r.right) - gutter_ - D(16));
        const int thumb = std::min(track, std::max(D(28), MulDiv(track, static_cast<int>(info.nPage), fullWidth)));
        const int x = gutter_ + MulDiv(std::max<LONG>(0, scroll.x), track - thumb,
                                       std::max(1, fullWidth - static_cast<int>(info.nPage)));
        horizontal_ = {x, bottom_ - D(8), x + thumb, bottom_ - D(3)};
        UiRoundedSurface(dc, horizontal_, c.border, c.border, D(3), dpi_);
    }
    Fill(dc, {0, bottom_, r.right, bottom_ + 1}, c.border);
    RECT footer{0, bottom_ + D(4), r.right, bottom_ + D(24)};
    footer.right -= D(96);
    TextAt(dc, ui_, footer, target_.empty() ? L"未选择终端" : target_, c.muted);
    RECT count = footer;
    count.left = r.right - D(88);
    TextAt(dc, ui_, count,
           range.first == range.second ? L"行 " + std::to_wstring(range.first + 1)
                                       : L"已选 " + std::to_wstring(range.second - range.first + 1) + L" 行",
           c.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT detail{0, bottom_ + D(24), r.right, r.bottom};
    std::wstring hint;
    if (running_)
        hint = L"已提交 " + std::to_wstring(sent_) + L"/" + std::to_wstring(total_) + L" · 停止仅取消后续发送";
    else if (range.first != range.second)
        hint =
            L"运行第 " + std::to_wstring(range.first + 1) + L"–" + std::to_wstring(range.second + 1) + L" 行完整命令";
    else
        hint = doubleClick_ ? L"双击正文运行 · Ctrl+Enter" : L"Ctrl+Enter 运行本行";
    TextAt(dc, ui_, detail, hint, c.muted);
}
void CommandNotebook::Scroll(int x, int y) {
    POINT p{std::max(0, x), std::max(0, y)};
    SendMessageW(editor_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&p));
    Refresh();
    InvalidateRect(editor_, nullptr, FALSE);
}
void CommandNotebook::Menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (doubleClick_ ? MF_CHECKED : 0), 1, L"正文双击运行");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < 4; ++i) {
        const unsigned values[] = {100, 500, 1500, 5000};
        AppendMenuW(menu, MF_STRING | (interval_ == values[i] ? MF_CHECKED : 0), 10 + i,
                    (L"发送间隔 " + std::to_wstring(values[i]) + L" ms").c_str());
    }
    RECT r{};
    GetWindowRect(more_, &r);
    auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN, r.right, r.bottom, 0, window_, nullptr);
    DestroyMenu(menu);
    if (choice == 1)
        doubleClick_ = !doubleClick_;
    if (choice >= 10 && choice < 14) {
        const unsigned values[] = {100, 500, 1500, 5000};
        interval_ = values[choice - 10];
    }
    if (choice && optionsChanged)
        optionsChanged();
    InvalidateRect(window_, nullptr, FALSE);
}
void CommandNotebook::SelectionInk(HDC dc) {
    if (selection_.cpMin == selection_.cpMax)
        return;
    const auto c = UiTheme(dark_);
    RECT client{};
    GetClientRect(editor_, &client);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, 0, 0, client.right, client.bottom);
    auto font = SelectObject(dc, mono_);
    SetBkColor(dc, c.accent);
    SetTextColor(dc, RGB(255, 255, 255));
    const auto range = CommandSelectedLines(lines_, selection_.cpMin, selection_.cpMax);
    for (size_t i = range.first; i <= range.second; ++i) {
        const auto a = std::max<size_t>(lines_[i].start, selection_.cpMin),
                   b = std::min<size_t>(lines_[i].end, selection_.cpMax);
        if (b <= a)
            continue;
        POINTL p{}, end{};
        SendMessageW(editor_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&p), a);
        if (p.y >= client.bottom)
            break;
        if (p.y + rowHeight_ < 0)
            continue;
        SendMessageW(editor_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&end), b);
        RECT box{p.x, p.y, end.x, p.y + rowHeight_};
        ExtTextOutW(dc, p.x, p.y, ETO_OPAQUE | ETO_CLIPPED, &box, text_.data() + a, static_cast<UINT>(b - a), nullptr);
    }
    SelectObject(dc, font);
    RestoreDC(dc, saved);
}
LRESULT CALLBACK CommandNotebook::ButtonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, ButtonProc, id);
    // Owner-drawn buttons must treat the second press as another ordinary
    // press/release, so rapid Run -> Stop is never swallowed by BN_DOUBLECLICKED.
    return DefSubclassProc(h, m == WM_LBUTTONDBLCLK ? WM_LBUTTONDOWN : m, w, l);
}
LRESULT CALLBACK CommandNotebook::EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto self = reinterpret_cast<CommandNotebook *>(data);
    if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, EditProc, id);
        return DefSubclassProc(h, m, w, l);
    }
    if (m == WM_DROPFILES) {
        SendMessageW(self->window_, m, w, l);
        return 0;
    }
    if (m == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && (w == 'S' || w == 'O')) {
        if (self->action)
            self->action(w == 'S' ? 2 : 3);
        return 0;
    }
    if (m == WM_CHAR && (w == 19 || w == 15))
        return 0;
    if (m == WM_CONTEXTMENU) {
        HMENU menu = CreatePopupMenu();
        const wchar_t *labels[] = {L"撤销", L"剪切", L"复制", L"粘贴", L"全选"};
        for (int i = 0; i < 5; ++i)
            AppendMenuW(menu, MF_STRING | ((self->running_ && (i == 0 || i == 1 || i == 3)) ? MF_GRAYED : 0), i + 1,
                        labels[i]);
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (point.x == -1 && point.y == -1) {
            GetCaretPos(&point);
            ClientToScreen(h, &point);
        }
        const auto picked = TrackPopupMenu(menu, TPM_RETURNCMD, point.x, point.y, 0, self->window_, nullptr);
        DestroyMenu(menu);
        const UINT messages[] = {WM_UNDO, WM_CUT, WM_COPY, WM_PASTE};
        if (picked >= 1 && picked <= 4)
            SendMessageW(h, messages[picked - 1], 0, 0);
        if (picked == 5) {
            CHARRANGE range{0, -1};
            SendMessageW(h, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        }
        return 0;
    }
    if (m == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        CHARRANGE range{0, -1};
        SendMessageW(h, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        return 0;
    }
    if ((m == WM_KEYDOWN && w == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000))) {
        if (self->action)
            self->action(1);
        return 0;
    }
    if (m == WM_CHAR && w == L'\r' && (GetKeyState(VK_CONTROL) & 0x8000))
        return 0;
    if (m == WM_LBUTTONDBLCLK && self->doubleClick_ && !self->running_) {
        POINTL p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const auto pos = SendMessageW(h, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&p));
        const auto row = CommandSelectedLines(self->lines_, static_cast<size_t>(std::max<LRESULT>(0, pos)),
                                              static_cast<size_t>(std::max<LRESULT>(0, pos)))
                             .first;
        if (self->runLine)
            self->runLine(row);
        return 0;
    }
    const auto result = DefSubclassProc(h, m, w, l);
    if (m == WM_PAINT) {
        HDC dc = GetDC(h);
        self->SelectionInk(dc);
        ReleaseDC(h, dc);
    }
    if (m == WM_MOUSEWHEEL || m == WM_HSCROLL || m == WM_VSCROLL || m == WM_KEYUP || m == WM_LBUTTONUP)
        self->Refresh();
    return result;
}
LRESULT CommandNotebook::Message(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        editor_ =
            CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NOOLEDRAGDROP | ES_MULTILINE | ES_AUTOVSCROLL |
                                ES_AUTOHSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(EditorId), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(editor_, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
        SendMessageW(editor_, EM_EXLIMITTEXT, 0, 4 * 1024 * 1024);
        SendMessageW(editor_, EM_SETTARGETDEVICE, 0, 1);
        SendMessageW(editor_, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE | ENM_SCROLL);
        SetWindowSubclass(editor_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
        DragAcceptFiles(editor_, TRUE);
        DragAcceptFiles(window_, TRUE);
        const wchar_t *labels[] = {L"运行", L"保存", L"导入", L"导出"};
        for (int i = 0; i < 4; ++i) {
            buttons_[i] =
                CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                window_, reinterpret_cast<HMENU>(FirstButton + i), GetModuleHandleW(nullptr), nullptr);
            UiStyleButton(buttons_[i]);
            SetWindowSubclass(buttons_[i], ButtonProc, 2, 0);
        }
        more_ = CreateWindowExW(0, L"BUTTON", L"更多选项", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0,
                                0, window_, reinterpret_cast<HMENU>(MoreId), GetModuleHandleW(nullptr), nullptr);
        UiStyleButton(more_);
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
        const wchar_t *tips[] = {L"运行当前行或选中行的完整命令 · Ctrl+Enter；发送期间可停止", L"保存命令笔记",
                                 L"导入 UTF-8 文本；也可直接拖入一个文件", L"导出命令笔记"};
        for (int i = 0; i < 4; ++i) {
            TOOLINFOW t{sizeof(t)};
            t.hwnd = window_;
            t.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            t.uId = reinterpret_cast<UINT_PTR>(buttons_[i]);
            t.lpszText = const_cast<wchar_t *>(tips[i]);
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&t));
        }
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, D(360));

        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_SETFOCUS:
        SetFocus(editor_);
        return 0;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(l) == editor_ && HIWORD(w) == EN_CHANGE) {
            if (!loading_)
                Refresh(true);
            return 0;
        }
        if (LOWORD(w) >= FirstButton && LOWORD(w) < FirstButton + 4 && HIWORD(w) == BN_CLICKED) {
            if (action)
                action(LOWORD(w) - FirstButton + 1);
            return 0;
        }
        if (LOWORD(w) == MoreId && HIWORD(w) == BN_CLICKED) {
            Menu();
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR *>(l)->hwndFrom == editor_) {
            if (reinterpret_cast<NMHDR *>(l)->code == EN_SELCHANGE)
                Refresh();
        }
        return 0;
    case WM_DROPFILES: {
        auto h = reinterpret_cast<HDROP>(w);
        std::wstring path;
        if (DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0) == 1) {
            const auto len = DragQueryFileW(h, 0, nullptr, 0);
            path.resize(len + 1);
            DragQueryFileW(h, 0, path.data(), len + 1);
            path.resize(len);
        }
        DragFinish(h);
        if (!path.empty() && drop)
            drop(path);
        return 0;
    }
    case WM_DRAWITEM: {
        const auto &it = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        UiBufferedPaint buffered(it.hDC, it.rcItem);
        HDC dc = buffered.Dc();
        auto c = UiTheme(dark_);
        Fill(dc, it.rcItem, c.surface);
        const int i = static_cast<int>(it.CtlID) - FirstButton;
        const bool more = it.CtlID == MoreId, enabled = IsWindowEnabled(it.hwndItem) != FALSE;
        auto v = UiButtonVisual(c, enabled, (it.itemState & ODS_SELECTED) != 0,
                                GetPropW(it.hwndItem, L"SerialCtl.PowerHover") != nullptr, i == 0 && !running_);
        UiRoundedSurface(dc, it.rcItem, v.fill, v.border, D(10), dpi_);
        if (more) {
            TextAt(dc, ui_, it.rcItem, L"···", v.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            const int width = it.rcItem.right - it.rcItem.left;
            const bool compact = width < D(76);
            const int x = compact ? (width - D(16)) / 2 : D(8);
            UiGlyph glyph = i == 0   ? (running_ ? UiGlyph::Stop : UiGlyph::Run)
                            : i == 1 ? UiGlyph::Save
                            : i == 2 ? UiGlyph::Import
                                     : UiGlyph::Export;
            UiDrawGlyph(dc, {x, D(8), x + D(16), D(24)}, v.ink, glyph);
            if (!compact) {
                RECT label = it.rcItem;
                label.left = D(30);
                label.right -= D(4);
                const wchar_t *labels[] = {running_ ? L"停止" : L"运行", L"保存", L"导入", L"导出"};
                TextAt(dc, ui_, label, labels[std::clamp(i, 0, 3)], v.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        UiButtonKeyboardFocus(dc, it.rcItem, it.itemState, dpi_, UiFocusInk(dark_, i == 0 && !running_));
        return TRUE;
    }
    case WM_MOUSEWHEEL:
        SendMessageW(editor_, m, w, l);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (p.x < gutter_ && p.y >= top_ && p.y < bottom_ - D(16) && !running_) {
            POINTL text{0, p.y - top_};
            const auto pos = SendMessageW(editor_, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&text));
            const auto row = CommandSelectedLines(lines_, std::max<LRESULT>(0, pos), std::max<LRESULT>(0, pos)).first;
            const auto range = CommandSelectedLines(lines_, selection_.cpMin, selection_.cpMax);
            if (p.x < D(16) && row >= range.first && row <= range.second &&
                !CommandLineText(text_, lines_[row]).empty()) {
                armedLine_ = static_cast<int>(row);
                SetCapture(window_);
            } else
                Selection(static_cast<LONG>(lines_[row].start), static_cast<LONG>(lines_[row].start));
            return 0;
        }
        if (PtInRect(&vertical_, p)) {
            scrollAxis_ = 2;
            scrollGrab_ = p.y - vertical_.top;
            SetCapture(window_);
            return 0;
        }
        if (PtInRect(&horizontal_, p)) {
            scrollAxis_ = 1;
            scrollGrab_ = p.x - horizontal_.left;
            SetCapture(window_);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (scrollAxis_) {
            POINT scroll{};
            SendMessageW(editor_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
            RECT r{};
            GetClientRect(editor_, &r);
            if (scrollAxis_ == 2) {
                const int full = static_cast<int>(lines_.size()) * rowHeight_,
                          track = bottom_ - top_ - D(16) - (vertical_.bottom - vertical_.top);
                Scroll(scroll.x, MulDiv(GET_Y_LPARAM(l) - top_ - scrollGrab_,
                                        std::max(0, full - static_cast<int>(r.bottom)), std::max(1, track)));
            } else {
                SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE};
                RECT view{};
                GetClientRect(editor_, &view);
                info.nMax = contentWidth_ - 1;
                info.nPage = static_cast<UINT>(view.right);
                RECT parent{};
                GetClientRect(window_, &parent);
                const int track = parent.right - gutter_ - D(16) - (horizontal_.right - horizontal_.left);
                Scroll(MulDiv(GET_X_LPARAM(l) - gutter_ - scrollGrab_,
                              std::max(0, info.nMax + 1 - static_cast<int>(info.nPage)), std::max(1, track)),
                       scroll.y);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (armedLine_ >= 0) {
            const int line = armedLine_;
            armedLine_ = -1;
            if (GetCapture() == window_)
                ReleaseCapture();
            POINTL p{0, GET_Y_LPARAM(l) - top_};
            const auto pos = SendMessageW(editor_, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&p));
            const auto row = CommandSelectedLines(lines_, std::max<LRESULT>(0, pos), std::max<LRESULT>(0, pos)).first;
            if (GET_X_LPARAM(l) >= 0 && GET_X_LPARAM(l) < D(16) && GET_Y_LPARAM(l) >= top_ &&
                GET_Y_LPARAM(l) < bottom_ - D(16) && static_cast<int>(row) == line && runLine)
                runLine(row);
            return 0;
        }
        [[fallthrough]];
    case WM_CAPTURECHANGED:
        armedLine_ = -1;
        if (scrollAxis_) {
            scrollAxis_ = 0;
            if (GetCapture() == window_)
                ReleaseCapture();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window_, &ps);
        RECT r{};
        GetClientRect(window_, &r);
        {
            UiBufferedPaint buffer(dc, r);
            Paint(buffer.Dc());
        }
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        Paint(reinterpret_cast<HDC>(w));
        return 0;
    }
    return DefWindowProcW(window_, m, w, l);
}
} // namespace serialctl
