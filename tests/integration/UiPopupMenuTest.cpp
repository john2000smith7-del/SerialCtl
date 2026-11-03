#include "UiPopupMenu.h"
#include "UiTestEvidence.h"
#include <iostream>
#include <stdexcept>
using namespace serialctl;
namespace {
void Check(bool value,const char* note){if(!value)throw std::runtime_error(note);}
struct Probe {
    HWND owner=nullptr;
    HMENU menu=nullptr;
    bool accept=false,dark=false;
    int dpi=96;
    DWORD started=0;
    unsigned positions=0;
    bool observed=false,region=false,nestedRejected=false,navigated=false;
    std::string error;
};
thread_local Probe* active=nullptr;
LRESULT CALLBACK Observe(int code,WPARAM w,LPARAM l) {
    if(code>=0&&active) {
        const auto& message=*reinterpret_cast<CWPSTRUCT*>(l);
        wchar_t name[24]{};GetClassNameW(message.hwnd,name,24);
        if(wcscmp(name,L"#32768")==0&&message.message==WM_WINDOWPOSCHANGED)++active->positions;
    }
    return CallNextHookEx(nullptr,code,w,l);
}
BOOL CALLBACK Find(HWND h,LPARAM value) {
    wchar_t name[24]{};GetClassNameW(h,name,24);
    if(wcscmp(name,L"#32768")==0&&IsWindowVisible(h)){*reinterpret_cast<HWND*>(value)=h;return FALSE;}
    return TRUE;
}
void Key(WORD code) {
    INPUT events[2]{};for(auto& event:events){event.type=INPUT_KEYBOARD;event.ki.wVk=code;}
    events[1].ki.dwFlags=KEYEVENTF_KEYUP;
    if(SendInput(2,events,sizeof(INPUT))!=2)throw std::runtime_error("native menu keyboard injection");
}
void CALLBACK Tick(HWND owner,UINT,UINT_PTR timer,DWORD) {
    auto* p=active;
    try {
        HWND menu=nullptr;EnumThreadWindows(GetCurrentThreadId(),Find,reinterpret_cast<LPARAM>(&menu));
        if(!menu) {
            if(GetTickCount()-p->started>3000)throw std::runtime_error("native menu did not become visible");
            return;
        }
        p->observed=true;
        HRGN region=CreateRectRgn(0,0,0,0);
        RECT box{};GetWindowRect(menu,&box);
        if(region) {
            p->region=GetWindowRgn(menu,region)!=ERROR&&!PtInRegion(region,0,0)&&
                PtInRegion(region,(box.right-box.left)/2,(box.bottom-box.top)/2);
            DeleteObject(region);
        }
        auto nested=CreatePopupMenu();AppendMenuW(nested,MF_STRING,99,L"Nested should be rejected");
        p->nestedRejected=UiPopupMenu::Track(nested,TPM_RETURNCMD,10,10,owner,UiFieldStyle{&p->dark,nullptr,p->dpi})==0;
        DestroyMenu(nested);
        if(p->accept&&!p->navigated){p->navigated=true;Key(VK_DOWN);return;}
        KillTimer(owner,timer);
        if(p->accept) {
            Check((GetMenuState(p->menu,1,MF_BYCOMMAND)&MF_HILITE)!=0,"native menu keyboard selection missing");
            RedrawWindow(menu,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
            RECT row{};Check(GetMenuItemRect(owner,p->menu,0,&row)!=FALSE,"native menu item bounds");
            POINT sample{row.left+MulDiv(20,p->dpi,96),(row.top+row.bottom)/2};ScreenToClient(menu,&sample);
            HDC dc=GetDC(menu);const auto fill=GetPixel(dc,sample.x,sample.y);ReleaseDC(menu,dc);
            Check(fill==UiTheme(p->dark).accent,"menu selection differs from shared combo accent");
            ui_test::SaveUiEvidence(menu,L"v219-popup-"+std::wstring(p->dark?L"dark-":L"light-")+std::to_wstring(p->dpi)+L"dpi.png");
            Key(VK_RETURN);
        } else Key(VK_ESCAPE);
    }catch(const std::exception& e){p->error=e.what();KillTimer(owner,timer);EndMenu();}
}
}
int main() {
    try {
        SetProcessDPIAware();INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
        Gdiplus::GdiplusStartupInput startup;ULONG_PTR token=0;Check(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"GDI+ startup");
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SerialCtlPopupRegression";RegisterClassW(&wc);
        const auto owner=CreateWindowExW(0,wc.lpszClassName,L"Actual native menu regression",WS_OVERLAPPEDWINDOW|WS_VISIBLE,40,40,700,500,nullptr,nullptr,wc.hInstance,nullptr);
        Check(owner!=nullptr,"menu owner");SetForegroundWindow(owner);UpdateWindow(owner);
        unsigned samples=0;
        for(bool dark:{false,true})for(int dpi:{96,144,192})for(bool accept:{true,false}) {
            const auto font=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
            auto menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"Analyze logs / 分析日志\nUse actual terminal evidence / 引用实际输出");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING|MF_GRAYED,2,L"Disabled / 禁用");
            AppendMenuW(menu,MF_STRING|MF_CHECKED,3,L"Checked / 已选");
            MENUITEMINFOW before{sizeof(before)};before.fMask=MIIM_FTYPE|MIIM_DATA;GetMenuItemInfoW(menu,0,TRUE,&before);
            Probe probe;probe.owner=owner;probe.menu=menu;probe.accept=accept;probe.dark=dark;probe.dpi=dpi;probe.started=GetTickCount();
            active=&probe;
            auto hook=SetWindowsHookExW(WH_CALLWNDPROC,Observe,nullptr,GetCurrentThreadId());
            Check(hook!=nullptr,"position observation hook");SetTimer(owner,219,80,Tick);
            const auto choice=UiPopupMenu::Track(menu,TPM_RETURNCMD,80,100,owner,UiFieldStyle{&dark,font,dpi});
            KillTimer(owner,219);UnhookWindowsHookEx(hook);active=nullptr;
            MENUITEMINFOW after{sizeof(after)};after.fMask=MIIM_FTYPE|MIIM_DATA;GetMenuItemInfoW(menu,0,TRUE,&after);
            DestroyMenu(menu);DeleteObject(font);
            if(!probe.error.empty())throw std::runtime_error(probe.error);
            Check(probe.observed&&probe.region,"real native menu rounded region was not applied");
            Check(probe.positions>0&&probe.positions<30,"native menu WINDOWPOS recursion");
            Check(probe.nestedRejected,"nested popup corrupted active owner context");
            Check(choice==(accept?1u:0u),"native menu keyboard selection/cancel changed");
            Check(before.fType==after.fType&&before.dwItemData==after.dwItemData,"native menu data/type not restored");
            ++samples;
        }
        DestroyWindow(owner);Gdiplus::GdiplusShutdown(token);
        std::cout<<"PASS: "<<samples<<" real native menu cycles, both themes, 96/144/192 renderer DPI, keyboard select/cancel, reentry and original data restoration; system DPI is unchanged\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
