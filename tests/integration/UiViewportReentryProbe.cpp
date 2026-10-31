#include "UiDialogViewport.h"
#include <iostream>

namespace {
bool armed = false, destroyed = false;
LRESULT CALLBACK CloseOnMove(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (message == WM_WINDOWPOSCHANGED && armed) {
        armed = false;
        const auto dialog = GetParent(h);
        destroyed = DestroyWindow(dialog) != FALSE;
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(h, CloseOnMove, id);
    return DefSubclassProc(h, message, w, l);
}
}
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token{}; Gdiplus::GdiplusStartup(&token, &input, nullptr);
    const auto root = CreateWindowExW(0, L"STATIC", L"Viewport lifetime probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        20, 20, 460, 2000, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND first = nullptr;
    for (int i=0; i<8; ++i) {
        auto field=CreateWindowExW(0,L"EDIT",L"Native field",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            16,40+i*180,360,28,root,reinterpret_cast<HMENU>(100+i),GetModuleHandleW(nullptr),nullptr);
        if(!first)first=field;
    }
    CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
        250,1800,96,36,root,reinterpret_cast<HMENU>(IDCANCEL),GetModuleHandleW(nullptr),nullptr);
    serialctl::UiDialogViewport::Fit(root,96);
    DWORD_PTR viewport{};
    if(!GetWindowSubclass(root,serialctl::UiFirstPaintProc,89,&viewport)) {
        // Fit installs its own private subclass. A reduced native height is the
        // observable setup condition; no access to the private viewport object.
        RECT bounds{};GetWindowRect(root,&bounds);
        if(bounds.bottom-bounds.top>=1800) { std::cerr<<"No overflow viewport\n";return 2; }
    }
    SetWindowSubclass(first,CloseOnMove,99,0);
    armed=true;
    std::cout<<"Entering native scroll; a child callback closes the owning dialog\n"<<std::flush;
    SendMessageW(root,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),0);
    if(IsWindow(root))DestroyWindow(root);
    Gdiplus::GdiplusShutdown(token);
    if(!destroyed) { std::cerr<<"Destruction callback was not exercised\n";return 3; }
    std::cout<<"PASS: native scroll returned safely after synchronous owner destruction\n";
    return 0;
}
