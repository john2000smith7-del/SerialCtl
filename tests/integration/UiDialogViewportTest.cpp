#include "UiDialogViewport.h"
#include <iostream>

using serialctl::UiDialogViewport;

namespace {
int failures = 0;
void Expect(bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
RECT Bounds(HWND h, HWND parent = nullptr) {
    RECT r{}; GetWindowRect(h, &r);
    if (parent) MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&r), 2);
    return r;
}
LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CTLCOLORDLG) return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    return DefWindowProcW(h, m, w, l);
}
}
int main() {
    SetProcessDPIAware();
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token{}; Gdiplus::GdiplusStartup(&token, &startup, nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SerialCtlViewportTest"; RegisterClassW(&wc);
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int full = monitor.rcWork.bottom - monitor.rcWork.top + 300;
    HWND root = CreateWindowExW(0, wc.lpszClassName, L"Viewport regression", WS_OVERLAPPEDWINDOW,
                                20, 20, 460, full, nullptr, nullptr, wc.hInstance, nullptr);
    auto child = [&](const wchar_t *type, int id, int y, int height) {
        return CreateWindowExW(0, type, L"Test", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              id == IDOK ? 320 : 16, y, 100, height, root,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
    };
    HWND first = child(L"EDIT", 100, 16, 28);
    HWND last = child(L"EDIT", 101, full - 200, 28);
    HWND overflowLabel = child(L"STATIC", 103, full - 160, 24);
    child(L"BUTTON", IDCANCEL, full - 120, 36);
    HWND accept = child(L"BUTTON", IDOK, full - 120, 36);
    HWND credit = child(L"STATIC", 102, full - 64, 20);
    ShowWindow(root, SW_SHOW);
    UiDialogViewport::Fit(root, 96);
    RECT fitted = Bounds(root), footer = Bounds(accept, root), before = Bounds(first, root), note = Bounds(credit, root);
    RECT client{}; GetClientRect(root, &client);
    Expect(fitted.top >= monitor.rcWork.top && fitted.bottom <= monitor.rcWork.bottom, "Dialog escaped work area");
    Expect(footer.top >= 0 && footer.bottom <= client.bottom, "Footer is not fully visible");
    Expect(note.top > footer.bottom && note.bottom <= client.bottom, "Credits must remain below the fixed footer");
    const auto labelBounds = Bounds(overflowLabel, root);
    Expect(labelBounds.top >= client.bottom || labelBounds.bottom <= footer.top - 12,
           "Offscreen content can paint over the fixed footer");
    SendMessageW(root, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    Expect(Bounds(first, root).top < before.top, "Wheel did not scroll overflow content");
    const RECT afterFooter = Bounds(accept, root);
    Expect(EqualRect(&footer, &afterFooter), "Scrolling moved the fixed action row");
    SetActiveWindow(root); SetFocus(last);
    RECT revealed = Bounds(last, root);
    Expect(revealed.top >= 0 && revealed.bottom <= footer.top - 12, "Keyboard focus did not reveal the complete field");
    SetFocus(first);
    Expect(Bounds(first, root).top >= 0, "Tabbing back did not restore the top field");
    Expect(GetDlgItem(root, 101) == last && GetParent(last) == root, "Native dialog control lookup changed");
    UiDialogViewport::Reset(root);
    Expect(Bounds(first, root).top == 16 && Bounds(last, root).top == full - 200,
           "Reset must restore natural positions before a dynamic dialog is re-laid out");
    UiDialogViewport::Fit(root, 96);
    const RECT refitFooter = Bounds(accept, root);
    Expect(EqualRect(&footer, &refitFooter), "Refitting accumulated a second footer shift");
    SetFocus(last);
    revealed = Bounds(last, root);
    Expect(revealed.top >= 0 && revealed.bottom <= refitFooter.top - 12,
           "Rebuilt viewport lost keyboard reveal");
    UiDialogViewport::Reset(root);
    SetWindowPos(root, nullptr, 0, 0, 460, 400, SWP_NOMOVE | SWP_NOZORDER);
    SetWindowPos(last, nullptr, 16, 150, 100, 28, SWP_NOZORDER);
    SetWindowPos(overflowLabel, nullptr, 16, 195, 100, 24, SWP_NOZORDER);
    SetWindowPos(accept, nullptr, 320, 260, 100, 36, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(root, IDCANCEL), nullptr, 16, 260, 100, 36, SWP_NOZORDER);
    SetWindowPos(credit, nullptr, 16, 310, 100, 20, SWP_NOZORDER);
    UiDialogViewport::Fit(root, 96);
    const RECT small = Bounds(root);
    SendMessageW(root, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    Expect(Bounds(first, root).top == 16 && Bounds(root).bottom == small.bottom,
           "Shrinking back to short content retained the overflow viewport");
    DestroyWindow(root);
    HWND hidden = CreateWindowExW(0, wc.lpszClassName, L"Hidden", WS_OVERLAPPEDWINDOW,
                                  0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    { serialctl::UiRedrawScope redraw(hidden); }
    Expect(!(GetWindowLongPtrW(hidden, GWL_STYLE) & WS_VISIBLE), "A redraw transaction exposed a hidden page");
    DestroyWindow(hidden);
    Gdiplus::GdiplusShutdown(token);
    return failures ? 1 : 0;
}
