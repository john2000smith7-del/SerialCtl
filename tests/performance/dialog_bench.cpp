// Actual desktop clicks; visible HWND and WM_NULL timings are not a pixel-presentation measurement.
// Wait beyond the system double-click interval: production intentionally coalesces rapid submits.
// Abort on a missed click/unclosed modal so later samples can never reuse an old dialog.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

struct Search { DWORD pid; HWND h=nullptr; bool dialog=false; bool requireVisible=true; bool requireTitle=true; };
static BOOL CALLBACK top(HWND h, LPARAM l) {
    auto &s=*reinterpret_cast<Search*>(l); DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    if(pid!=s.pid || (s.requireVisible && !IsWindowVisible(h))) return TRUE;
    wchar_t name[128]={}, title[256]={}; GetClassNameW(h,name,128); GetWindowTextW(h,title,256);
    if ((!s.dialog && wcscmp(name,L"SerialCtlWin7NativeWindowV3")==0) ||
        (s.dialog && wcscmp(name,L"#32770")==0 && (!s.requireTitle || title[0]))) {s.h=h; return FALSE;}
    return TRUE;
}
static HWND find(DWORD pid,bool dialog,bool visible=true,bool title=true) { Search s{pid,nullptr,dialog,visible,title}; EnumWindows(top,reinterpret_cast<LPARAM>(&s));return s.h; }
static HWND await(DWORD pid,bool dialog,DWORD ms) {ULONGLONG end=GetTickCount64()+ms;while(GetTickCount64()<end){if(auto h=find(pid,dialog))return h;Sleep(2);}return nullptr;}
static bool click(HWND h) {RECT r{}; if(!h||!IsWindowVisible(h)||!GetWindowRect(h,&r))return false; SetCursorPos((r.left+r.right)/2,(r.top+r.bottom)/2);mouse_event(MOUSEEVENTF_LEFTDOWN,0,0,0,0);Sleep(10);mouse_event(MOUSEEVENTF_LEFTUP,0,0,0,0);return true;}
int wmain(int argc,wchar_t **argv) {
    if(argc!=4)return 2;
    const bool dark=argc>=3 && wcscmp(argv[2],L"dark")==0;
    wchar_t cmd[4096]{}; swprintf(cmd,4096,L"\"%ls\"",argv[1]);
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,cmd,nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){printf("create_failed=%lu\n",GetLastError());return 3;}
    WaitForInputIdle(pi.hProcess,10000); HWND main=await(pi.dwProcessId,false,20000);
    if(!main){printf("main_missing\n");TerminateProcess(pi.hProcess,3);return 3;}
    SetWindowPos(main,nullptr,0,0,1200,800,SWP_NOZORDER);SetForegroundWindow(main);Sleep(400);
    if(dark){if(!click(GetDlgItem(main,104))){TerminateProcess(pi.hProcess,9);return 9;}Sleep(GetDoubleClickTime()+100);}
    if(!click(GetDlgItem(main,304))){TerminateProcess(pi.hProcess,13);return 13;}
    HWND ai=await(pi.dwProcessId,true,10000);
    if(!ai){TerminateProcess(pi.hProcess,14);return 14;}
    PostMessageW(ai,WM_CLOSE,0,0);
    const auto aiClose=GetTickCount64()+5000;
    while(IsWindow(ai)&&GetTickCount64()<aiClose)Sleep(5);
    if(IsWindow(ai)){TerminateProcess(pi.hProcess,15);return 15;}
    Sleep(GetDoubleClickTime()+100);
    if(!click(GetDlgItem(main,300))){TerminateProcess(pi.hProcess,10);return 10;}
    const auto connectDeadline=GetTickCount64()+10000;
    while(GetTickCount64()<connectDeadline && SendMessageW(GetDlgItem(main,106),LB_GETCOUNT,0,0)<2)Sleep(10);
    HWND terminal=GetDlgItem(main,107);
    if(!terminal || SendMessageW(GetDlgItem(main,106),LB_GETCOUNT,0,0)<2){TerminateProcess(pi.hProcess,11);return 11;}
    std::wstring command=L"\""+std::wstring(argv[3])+L"\"";
    // Fixture setup only. Timed connection-button actions below still use the real mouse.
    for(const auto key:command)SendMessageW(terminal,WM_CHAR,key,0);
    SendMessageW(terminal,WM_CHAR,13,0);
    Sleep(2500);
    const auto streamDeadline=GetTickCount64()+15000;
    while(!GetPropW(terminal,L"SerialCtl.PaintSample") && GetTickCount64()<streamDeadline)Sleep(10);
    if(!GetPropW(terminal,L"SerialCtl.PaintSample")){printf("stream_not_painted\n");TerminateProcess(pi.hProcess,12);return 12;}
    struct Spec {const char *name; int id;}; const Spec all[]={{"ssh",100},{"serial",101},{"telnet",102},{"remote",103},{"ai",304}};
    for(const auto &spec:all) {
       for(int rep=0;rep<5;rep++) {
          HWND b=GetDlgItem(main,spec.id);SetForegroundWindow(main);Sleep(GetDoubleClickTime()+100);
          const auto paintBefore=reinterpret_cast<UINT_PTR>(GetPropW(GetDlgItem(main,107),L"SerialCtl.PaintSample"));
          ULONGLONG start=GetTickCount64();if(!click(b)){printf("%s,%d,click_failed\n",spec.name,rep);TerminateProcess(pi.hProcess,5);return 5;}ULONGLONG eventSent=GetTickCount64();
          HWND dialog=nullptr;ULONGLONG created=0,visible=0,caption=0;const ULONGLONG deadline=start+12000;
          while(GetTickCount64()<deadline) {
            if(!dialog){dialog=find(pi.dwProcessId,true,false,false);if(dialog)created=GetTickCount64();}
            if(dialog&&!visible&&IsWindowVisible(dialog))visible=GetTickCount64();
            if(dialog&&visible){wchar_t title[256]{};GetWindowTextW(dialog,title,256);if(title[0]){caption=GetTickCount64();break;}}
            Sleep(1);
          }
          if(!dialog || !visible || !caption){printf("%s,%d,timeout,created_ms=%llu\n",spec.name,rep,created?created-start:0);TerminateProcess(pi.hProcess,6);return 6;}
          DWORD_PTR result=0;BOOL ready=SendMessageTimeoutW(dialog,WM_NULL,0,0,SMTO_ABORTIFHUNG,5000,&result);ULONGLONG done=GetTickCount64();if(!ready){TerminateProcess(pi.hProcess,7);return 7;}
          const auto paintAfter=reinterpret_cast<UINT_PTR>(GetPropW(GetDlgItem(main,107),L"SerialCtl.PaintSample"));
          printf("%s,%d,event_ms=%llu,created_ms=%llu,visible_ms=%llu,caption_ms=%llu,responsive_ms=%llu,responsive=%d,paint_delta=%llu\n",spec.name,rep,eventSent-start,created-start,visible-start,caption-start,done-start,ready?1:0,paintAfter-paintBefore);fflush(stdout);
          PostMessageW(dialog,WM_CLOSE,0,0);ULONGLONG end=GetTickCount64()+5000;while(IsWindow(dialog)&&GetTickCount64()<end)Sleep(5);if(IsWindow(dialog)){TerminateProcess(pi.hProcess,8);return 8;}Sleep(200);
       }
    }
    PostMessageW(main,WM_CLOSE,0,0);
    if(WaitForSingleObject(pi.hProcess,10000)==WAIT_TIMEOUT){TerminateProcess(pi.hProcess,4);return 4;}
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 0;
}
