#include "UiStyle.h"
#include <iostream>
#include <stdexcept>

using namespace serialctl;
namespace {
bool dark = false;
UiFieldStyle style{&dark, nullptr, 96};
HBRUSH fieldBrush = nullptr;
bool rebuildOnSelection = false;
unsigned selectionNotifications = 0;
void Require(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
void AddChoices(HWND combo) {
    for (const auto *text : {L"First / 第一项", L"Second / 第二项", L"Third / 第三项"})
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}
LRESULT CALLBACK ParentProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_DRAWITEM) {
        const auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        if (item.CtlType == ODT_COMBOBOX) {
            UiDrawComboItem(item, style);
            return TRUE;
        }
    }
    if (message == WM_MEASUREITEM) {
        reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = MulDiv(28, style.dpi, 96);
        return TRUE;
    }
    if (message == WM_COMMAND && HIWORD(w) == CBN_SELCHANGE) {
        ++selectionNotifications;
        if (rebuildOnSelection) {
            rebuildOnSelection = false;
            auto combo = reinterpret_cast<HWND>(l);
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            AddChoices(combo);
            SendMessageW(combo, CB_SETCURSEL, 1, 0);
        }
        return 0;
    }
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORLISTBOX) {
        SetTextColor(reinterpret_cast<HDC>(w), UiTheme(dark).text);
        SetBkColor(reinterpret_cast<HDC>(w), UiTheme(dark).field);
        return reinterpret_cast<LRESULT>(fieldBrush);
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        RECT bounds{};
        GetClientRect(h, &bounds);
        FillRect(dc, &bounds, fieldBrush);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, message, w, l);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
std::vector<DWORD> Snapshot(HWND combo) {
    RECT bounds{};
    GetClientRect(combo, &bounds);
    HDC dc = GetDC(combo), buffer = CreateCompatibleDC(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = bounds.right;
    info.bmiHeader.biHeight = -bounds.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !buffer || !bitmap || !pixels) {
        if (bitmap)
            DeleteObject(bitmap);
        if (buffer)
            DeleteDC(buffer);
        if (dc)
            ReleaseDC(combo, dc);
        throw std::runtime_error("Cannot capture visible combo pixels");
    }
    auto previous = SelectObject(buffer, bitmap);
    const bool copied = BitBlt(buffer, 0, 0, bounds.right, bounds.bottom, dc, 0, 0, SRCCOPY) != FALSE;
    GdiFlush();
    const auto *source = static_cast<const DWORD *>(pixels);
    std::vector<DWORD> result(source, source + bounds.right * bounds.bottom);
    for (auto &pixel : result)
        pixel &= 0x00ffffff;
    SelectObject(buffer, previous);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    ReleaseDC(combo, dc);
    Require(copied, "Cannot copy visible combo pixels");
    return result;
}
void CheckSettled(HWND combo, const char *action) {
    // Do not pump WM_PAINT or change focus before this capture: the old native
    // inline paint remains rectangular until a later repaint/focus transition.
    const auto immediate = Snapshot(combo);
    RedrawWindow(combo, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    const auto canonical = Snapshot(combo);
    if (immediate != canonical) {
        std::cerr << "theme=" << (dark ? "dark" : "light") << " dpi=" << style.dpi << " action=" << action << '\n';
        throw std::runtime_error("Selection/focus left native pixels over the themed field until a later repaint");
    }
}
void RunCase(HWND parent, bool ownerDrawn) {
    const auto instance = GetModuleHandleW(nullptr);
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                     (ownerDrawn ? CBS_OWNERDRAWFIXED | CBS_HASSTRINGS : 0),
                                 20, 40, 440, 200, parent, reinterpret_cast<HMENU>(10), instance, nullptr);
    Require(combo != nullptr, "Cannot create native dropdown list");
    struct Cleanup {
        HWND h;
        ~Cleanup() {
            DestroyWindow(h);
        }
    } cleanup{combo};
    SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(style.font), FALSE);
    UiStyleField(combo, &style);
    AddChoices(combo);
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    UiLayoutBatch layout;
    layout.Move(combo, 20, 40, MulDiv(220, style.dpi, 96), MulDiv(28, style.dpi, 96));
    layout.ComboVisibleHeight(combo, MulDiv(28, style.dpi, 96));
    layout.Commit();
    SetFocus(combo);
    Pump();
    RECT normalized{};
    GetWindowRect(combo, &normalized);
    Require(normalized.bottom - normalized.top == MulDiv(28, style.dpi, 96),
            "Native dropdown visible height must match the shared 28 px field at every DPI");
    RedrawWindow(combo, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    CheckSettled(combo, "initial");
    SendMessageW(combo, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
    CheckSettled(combo, "hover entered");
    RECT field{};
    GetClientRect(combo, &field);
    SendMessageW(combo, WM_MOUSEMOVE, 0, MAKELPARAM(field.right - 4, field.bottom / 2));
    CheckSettled(combo, "hover crossed to arrow");
    SendMessageW(combo, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
    CheckSettled(combo, "hover returned to label");
    SendMessageW(combo, WM_MOUSELEAVE, 0, 0);
    CheckSettled(combo, "hover left");
    const auto notifications = selectionNotifications;
    SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
    CheckSettled(combo, "keyboard selection");
    Require(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1, "Keyboard selection changed native behavior");
    Require(selectionNotifications > notifications, "Native selection notification was lost");
    Pump();
    CheckSettled(combo, "keyboard after queued messages");
    SendMessageW(combo, CB_SETCURSEL, 2, 0);
    CheckSettled(combo, "programmatic selection");
    SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
    Pump();
    COMBOBOXINFO info{sizeof(info)};
    Require(GetComboBoxInfo(combo, &info) && IsWindowVisible(info.hwndList), "Dropdown did not open");
    RECT row{};
    SendMessageW(info.hwndList, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&row));
    const auto point = MAKELPARAM(10, (row.top + row.bottom) / 2);
    SendMessageW(info.hwndList, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(info.hwndList, WM_LBUTTONUP, 0, point);
    Require(!SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0), "Native mouse selection did not close dropdown");
    CheckSettled(combo, "mouse selection before focus loss");
    Pump();
    CheckSettled(combo, "mouse after queued messages");
    rebuildOnSelection = true;
    SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
    CheckSettled(combo, "list rebuilt inside selection callback");
    Require(!rebuildOnSelection && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1, "Reentrant selection rebuild failed");
    EnableWindow(combo, FALSE);
    CheckSettled(combo, "disabled");
    EnableWindow(combo, TRUE);
    SetFocus(combo);
    CheckSettled(combo, "focus gained");
    SetFocus(parent);
    CheckSettled(combo, "focus lost");
    SetFocus(combo);
    const auto resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 30; ++i) {
        SendMessageW(combo, CB_SETCURSEL, i % 3, 0);
        CheckSettled(combo, "repeated selection");
    }
    Require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == resources,
            "Repeated field painting leaked GDI resources");
}
} // namespace
int main() {
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
        return 1;
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = ParentProc;
    cls.lpszClassName = L"SerialCtlComboPaintTest";
    RegisterClassW(&cls);
    auto parent =
        CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"Combo paint regression", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        20, 20, 700, 500, nullptr, nullptr, cls.hInstance, nullptr);
    int result = 0;
    try {
        Require(parent != nullptr, "Cannot create test window");
        for (bool palette : {false, true}) {
            dark = palette;
            fieldBrush = CreateSolidBrush(UiTheme(dark).field);
            for (int dpi : {96, 144, 192}) {
                style.dpi = dpi;
                style.font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                         0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                for (bool owner : {false, true})
                    RunCase(parent, owner);
                DeleteObject(style.font);
                style.font = nullptr;
            }
            DeleteObject(fieldBrush);
            fieldBrush = nullptr;
        }
        std::cout << "Combo paint: native mouse/keyboard selection, programmatic/reentrant updates, retained focus, "
                     "disabled state, two palettes, 96/144/192 renderer scaling and GDI lifetime passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    DestroyWindow(parent);
    if (style.font)
        DeleteObject(style.font);
    if (fieldBrush)
        DeleteObject(fieldBrush);
    Gdiplus::GdiplusShutdown(token);
    return result;
}
