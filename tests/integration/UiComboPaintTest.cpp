#include "UiStyle.h"
#include "UiPathField.h"
#include "UiTestEvidence.h"
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
std::vector<DWORD> Snapshot(HWND combo);
void CheckSettled(HWND combo,const char* action);
struct ReleaseProbe { HWND combo=nullptr; bool checked=false; std::string failure; } releaseProbe;
LRESULT CALLBACK MouseReleaseProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    const auto result=DefSubclassProc(h,m,w,l);
    if(m==WM_LBUTTONUP && IsWindow(releaseProbe.combo) && !SendMessageW(releaseProbe.combo,CB_GETDROPPEDSTATE,0,0)) {
        releaseProbe.checked=true;
        try { CheckSettled(releaseProbe.combo,"actual mouse release before queued paints"); }
        catch(const std::exception& error) { releaseProbe.failure=error.what(); }
    }
    if(m==WM_NCDESTROY) RemoveWindowSubclass(h,MouseReleaseProc,id);
    return result;
}
void PumpFor(unsigned ms) {
    const auto until=GetTickCount64()+ms;
    do { Pump(); Sleep(1); } while(GetTickCount64()<until);
}
void RealClick(int x,int y) {
    const int left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width=GetSystemMetrics(SM_CXVIRTUALSCREEN),height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in[3]{};
    in[0].type=INPUT_MOUSE;in[0].mi.dx=MulDiv(x-left,65535,std::max(1,width-1));in[0].mi.dy=MulDiv(y-top,65535,std::max(1,height-1));
    in[0].mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
    in[1].type=in[2].type=INPUT_MOUSE;in[1].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;in[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    Require(SendInput(3,in,sizeof(INPUT))==3,"Actual mouse input was rejected");
    PumpFor(60);
}
void RunLongList(HWND parent,bool editable) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(parent,MONITOR_DEFAULTTONEAREST),&monitor);
    SetWindowPos(parent,HWND_TOPMOST,monitor.rcWork.left+20,monitor.rcWork.bottom-420,700,400,SWP_SHOWWINDOW);
    auto combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|
        (editable?CBS_DROPDOWN|CBS_AUTOHSCROLL:CBS_DROPDOWNLIST)|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
        20,40,320,240,parent,reinterpret_cast<HMENU>(11),GetModuleHandleW(nullptr),nullptr);
    Require(combo!=nullptr,"Cannot create long native list");
    SendMessageW(combo,WM_SETFONT,reinterpret_cast<WPARAM>(style.font),FALSE);UiStyleField(combo,&style);
    for(unsigned i=0;i<500;++i) {auto name=L"project-"+std::to_wstring(i);SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));}
    SendMessageW(combo,CB_SETCURSEL,0,0);
    UiLayoutBatch b;b.Move(combo,20,40,320,240);b.ComboVisibleHeight(combo,28);b.Commit();
    SetForegroundWindow(parent);PumpFor(50);
    for(unsigned i=0;i<6;++i) {
        RECT field{};GetWindowRect(combo,&field);
        RealClick(field.right-14,(field.top+field.bottom)/2);
        COMBOBOXINFO info{sizeof(info)};GetComboBoxInfo(combo,&info);
        Require(SendMessageW(combo,CB_GETDROPPEDSTATE,0,0)!=0,"Actual arrow click did not open dropdown");
        RECT drop{};GetWindowRect(info.hwndList,&drop);
        Require(drop.top>=field.bottom,"Shared painter moved a native downward drop above its opening click");
        Require(SendMessageW(info.hwndList,LB_GETTOPINDEX,0,0)==0,"Opening click scrolled a long list to its final rows");
        Require(SendMessageW(combo,CB_GETCURSEL,0,0)==0,"Opening click changed the selection");
        RECT row{};SendMessageW(info.hwndList,LB_GETITEMRECT,1,reinterpret_cast<LPARAM>(&row));
        POINT p{30,(row.top+row.bottom)/2};ClientToScreen(info.hwndList,&p);RealClick(p.x,p.y);
        Require(SendMessageW(combo,CB_GETCURSEL,0,0)==1 && !SendMessageW(combo,CB_GETDROPPEDSTATE,0,0),"Actual list click did not commit precisely the clicked row");
        SendMessageW(combo,CB_SETCURSEL,0,0);
    }
    DestroyWindow(combo);
    auto path=UiPathField::Create(parent,12,20,40,320,40,&style);
    const auto value=L"/home/linux/"+std::wstring(200,L'x')+L"/firmware-source";
    SetWindowTextW(path,value.c_str());RedrawWindow(path,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
    SendMessageW(path,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,15));
    RedrawWindow(path,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);const auto before=Snapshot(path);
    ui_test::SaveUiEvidence(path,dark?L"v215-path-dark-start.png":L"v215-path-light-start.png");
    SendMessageW(path,WM_KEYDOWN,VK_END,0);RedrawWindow(path,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
    ui_test::SaveUiEvidence(path,dark?L"v215-path-dark-end.png":L"v215-path-light-end.png");
    Require(before!=Snapshot(path),"Long path has no horizontal viewport movement");
    std::vector<wchar_t> actual(value.size()+1);GetWindowTextW(path,actual.data(),static_cast<int>(actual.size()));
    Require(value==actual.data(),"Horizontal viewing changed the stored full path");DestroyWindow(path);
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
void RunCase(HWND parent, bool ownerDrawn, bool composer = false) {
    const auto initialNotifications = selectionNotifications;
    // Connection forms assemble their controls before the parent is shown.
    // Deferred hidden paints must still produce a correct first visible field.
    ShowWindow(parent, SW_HIDE);
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
    if (composer) SetPropW(combo, L"SerialCtl.Composer", reinterpret_cast<HANDLE>(1));
    AddChoices(combo);
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    UiLayoutBatch layout;
    layout.Move(combo, 20, 40, MulDiv(220, style.dpi, 96), MulDiv(28, style.dpi, 96));
    layout.ComboVisibleHeight(combo, MulDiv(28, style.dpi, 96));
    layout.Commit();
    ShowWindow(parent, SW_SHOW);
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
    // At 200% scaling the native reserved drop may show only two rows. It
    // correctly scrolls to selected row 2 when opening; reveal row 0 before
    // asking this immediate-paint test to click that row.
    SendMessageW(info.hwndList,LB_SETTOPINDEX,0,0);
    RECT row{};
    SendMessageW(info.hwndList, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&row));
    POINT actualPoint{10,(row.top+row.bottom)/2};ClientToScreen(info.hwndList,&actualPoint);
    releaseProbe={combo,false,{}};
    SetWindowSubclass(combo,MouseReleaseProc,99,0);
    SetWindowSubclass(info.hwndList,MouseReleaseProc,99,0);
    RealClick(actualPoint.x,actualPoint.y);
    RemoveWindowSubclass(combo,MouseReleaseProc,99);
    RemoveWindowSubclass(info.hwndList,MouseReleaseProc,99);
    Require(!SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0), "Native mouse selection did not close dropdown");
    if(SendMessageW(combo,CB_GETCURSEL,0,0)!=0) {
        RECT drop{};GetWindowRect(info.hwndList,&drop);
        std::cerr<<"click theme="<<dark<<" dpi="<<style.dpi<<" owner="<<ownerDrawn<<" composer="<<composer
                 <<" selected="<<SendMessageW(combo,CB_GETCURSEL,0,0)<<" top="<<SendMessageW(info.hwndList,LB_GETTOPINDEX,0,0)
                 <<" row="<<row.top<<","<<row.bottom<<" point="<<actualPoint.x<<","<<actualPoint.y
                 <<" drop="<<drop.left<<","<<drop.top<<","<<drop.right<<","<<drop.bottom
                 <<" capture="<<GetCapture()<<"\n";
        throw std::runtime_error("Actual click selected a different row from its cursor");
    }
    Require(releaseProbe.checked && releaseProbe.failure.empty(),"Actual selection left noncanonical field pixels before queued paints");
    CheckSettled(combo, "mouse selection before focus loss");
    Pump();
    CheckSettled(combo, "mouse after queued messages");
    rebuildOnSelection = true;
    SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
    CheckSettled(combo, "list rebuilt inside selection callback");
    if(rebuildOnSelection || SendMessageW(combo,CB_GETCURSEL,0,0)!=1) {
        std::cerr<<"reentrant theme="<<dark<<" dpi="<<style.dpi<<" owner="<<ownerDrawn<<" composer="<<composer
                 <<" selected="<<SendMessageW(combo,CB_GETCURSEL,0,0)<<" notification="<<!rebuildOnSelection<<'\n';
        throw std::runtime_error("Reentrant selection rebuild failed");
    }
    EnableWindow(combo, FALSE);
    CheckSettled(combo, "disabled");
    EnableWindow(combo, TRUE);
    SetFocus(combo);
    CheckSettled(combo, "focus gained");
    if (composer) {
        RECT bounds{}; GetClientRect(combo, &bounds);
        const auto pixels = Snapshot(combo);
        const int edge = MulDiv(2, style.dpi, 96);
        for (int y = 0; y < bounds.bottom; ++y) for (int x = 0; x < bounds.right; ++x) {
            if (x >= edge && x < bounds.right-edge && y >= edge && y < bounds.bottom-edge) continue;
            const DWORD pixel = pixels[static_cast<size_t>(y)*bounds.right+x];
            const int red=(pixel>>16)&255, green=(pixel>>8)&255, blue=pixel&255;
            Require(!(blue>red+20 && blue>green+20), "Composer target retained a nested blue focus frame");
        }
        Require(selectionNotifications > initialNotifications, "Frameless composer must retain native selection notifications");
    }
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
                RunCase(parent, true, true);
                if(dpi==96) { RunLongList(parent,false); RunLongList(parent,true); }
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
