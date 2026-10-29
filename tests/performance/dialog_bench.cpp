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
struct ControlSearch {int id; HWND out=nullptr;};
static BOOL CALLBACK child(HWND h,LPARAM l) {auto &s=*reinterpret_cast<ControlSearch*>(l);if(GetDlgCtrlID(h)==s.id&&IsWindowVisible(h)){s.out=h;return FALSE;}return TRUE;}
static BOOL CALLBACK pet(HWND h,LPARAM l) {
  auto &s=*reinterpret_cast<Search*>(l);DWORD pid=0;GetWindowThreadProcessId(h,&pid);wchar_t cls[128]{};GetClassNameW(h,cls,128);
  if(pid==s.pid && IsWindowVisible(h) && wcscmp(cls,L"SerialCtlAiPetPane")==0){s.h=h;return FALSE;}return TRUE;
}
int wmain(int argc,wchar_t **argv) {
    if(argc<2 || argc>3)return 2;
    const bool loaded=argc==3 && wcscmp(argv[2],L"loaded")==0;
    wchar_t cmd[4096]{}; swprintf(cmd,4096,L"\"%ls\"",argv[1]);
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,cmd,nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){printf("create_failed=%lu\n",GetLastError());return 3;}
    WaitForInputIdle(pi.hProcess,10000); HWND main=await(pi.dwProcessId,false,20000);
    if(!main){printf("main_missing\n");TerminateProcess(pi.hProcess,3);return 3;}
    SetWindowPos(main,nullptr,0,0,1200,800,SWP_NOZORDER);SetForegroundWindow(main);Sleep(400);
    if(loaded) {
       click(GetDlgItem(main,300));
       HWND list=GetDlgItem(main,106);ULONGLONG wait=GetTickCount64()+12000;
       while(GetTickCount64()<wait && SendMessageW(list,LB_GETCOUNT,0,0)<1) Sleep(10);
       HWND term=GetDlgItem(main,107);
       if(SendMessageW(list,LB_GETCOUNT,0,0)<1||!term){printf("load_setup_failed\n");PostMessageW(main,WM_CLOSE,0,0);return 6;}
       Sleep(300);
       const wchar_t* cmdtext=L"for /L %i in (1,1,100000) do @echo SERIALCTL_BENCH_%i";
       for(const wchar_t* p=cmdtext;*p;++p)SendMessageW(term,WM_CHAR,*p,0);
       SendMessageW(term,WM_CHAR,13,0); Sleep(100);
    }
    struct Spec {const char *name; int id;}; const Spec all[]={{"ssh",100},{"serial",101},{"telnet",102},{"remote",103},{"ai",304}};
    for(const auto &spec:all) {
       for(int rep=0;rep<3;rep++) {
          HWND b=GetDlgItem(main,spec.id);SetForegroundWindow(main);Sleep(100);
          const auto paintBefore=reinterpret_cast<UINT_PTR>(GetPropW(GetDlgItem(main,107),L"SerialCtl.PaintSample"));
          ULONGLONG start=GetTickCount64();if(!click(b)){printf("%s,%d,click_failed\n",spec.name,rep);continue;}ULONGLONG eventSent=GetTickCount64();
          HWND dialog=nullptr;ULONGLONG created=0,visible=0,caption=0;const ULONGLONG deadline=start+12000;
          while(GetTickCount64()<deadline) {
            if(!dialog){dialog=find(pi.dwProcessId,true,false,false);if(dialog)created=GetTickCount64();}
            if(dialog&&!visible&&IsWindowVisible(dialog))visible=GetTickCount64();
            if(dialog&&visible){wchar_t title[256]{};GetWindowTextW(dialog,title,256);if(title[0]){caption=GetTickCount64();break;}}
            Sleep(1);
          }
          if(!dialog || !visible || !caption){printf("%s,%d,timeout,created_ms=%llu\n",spec.name,rep,created?created-start:0);continue;}
          DWORD_PTR result=0;BOOL ready=SendMessageTimeoutW(dialog,WM_NULL,0,0,SMTO_ABORTIFHUNG,5000,&result);ULONGLONG done=GetTickCount64();
          const auto paintAfter=reinterpret_cast<UINT_PTR>(GetPropW(GetDlgItem(main,107),L"SerialCtl.PaintSample"));
          printf("%s,%d,event_ms=%llu,created_ms=%llu,visible_ms=%llu,caption_ms=%llu,responsive_ms=%llu,responsive=%d,paint_delta=%llu\n",spec.name,rep,eventSent-start,created-start,visible-start,caption-start,done-start,ready?1:0,paintAfter-paintBefore);fflush(stdout);
          PostMessageW(dialog,WM_CLOSE,0,0);ULONGLONG end=GetTickCount64()+5000;while(IsWindow(dialog)&&GetTickCount64()<end)Sleep(5);Sleep(200);
       }
    }
    Search petState{pi.dwProcessId};EnumWindows(pet,reinterpret_cast<LPARAM>(&petState));
    printf("pet_visible_after_ai=%d\n",petState.h?1:0);fflush(stdout);
    for(int rep=0;rep<3;rep++){
      SetForegroundWindow(main);Sleep(100);ULONGLONG start=GetTickCount64();if(!click(GetDlgItem(main,100))){printf("pet_ssh,%d,click_failed\n",rep);continue;}ULONGLONG eventSent=GetTickCount64();
      HWND dialog=nullptr;ULONGLONG created=0,visible=0,caption=0;const ULONGLONG deadline=start+12000;
      while(GetTickCount64()<deadline){
       if(!dialog){dialog=find(pi.dwProcessId,true,false,false);if(dialog)created=GetTickCount64();}
       if(dialog&&!visible&&IsWindowVisible(dialog))visible=GetTickCount64();
       if(dialog&&visible){wchar_t title[256]{};GetWindowTextW(dialog,title,256);if(title[0]){caption=GetTickCount64();break;}} Sleep(1);
      }
      if(!dialog||!visible||!caption){printf("pet_ssh,%d,timeout\n",rep);continue;}
      DWORD_PTR result=0;BOOL ready=SendMessageTimeoutW(dialog,WM_NULL,0,0,SMTO_ABORTIFHUNG,5000,&result);ULONGLONG done=GetTickCount64();
      printf("pet_ssh,%d,event_ms=%llu,created_ms=%llu,visible_ms=%llu,caption_ms=%llu,responsive_ms=%llu,responsive=%d\n",rep,eventSent-start,created-start,visible-start,caption-start,done-start,ready?1:0);fflush(stdout);
      PostMessageW(dialog,WM_CLOSE,0,0);ULONGLONG end=GetTickCount64()+5000;while(IsWindow(dialog)&&GetTickCount64()<end)Sleep(5);Sleep(200);
    }
    SetForegroundWindow(main);Sleep(100);click(GetDlgItem(main,301));Sleep(500);
    ControlSearch s{10,nullptr};EnumChildWindows(main,child,reinterpret_cast<LPARAM>(&s));
    for(int rep=0;rep<3;rep++){
      if(!s.out){printf("power,%d,connect_button_missing\n",rep);continue;}
      ULONGLONG start=GetTickCount64();if(!click(s.out)){printf("power,%d,click_failed\n",rep);continue;}
      HWND dialog=await(pi.dwProcessId,true,12000);ULONGLONG seen=GetTickCount64();
      if(!dialog){printf("power,%d,timeout\n",rep);continue;}
      DWORD_PTR result=0;BOOL ready=SendMessageTimeoutW(dialog,WM_NULL,0,0,SMTO_ABORTIFHUNG,5000,&result);ULONGLONG done=GetTickCount64();
      printf("power,%d,visible_ms=%llu,responsive_ms=%llu,responsive=%d\n",rep,seen-start,done-start,ready?1:0);fflush(stdout);
      PostMessageW(dialog,WM_CLOSE,0,0);ULONGLONG end=GetTickCount64()+5000;while(IsWindow(dialog)&&GetTickCount64()<end)Sleep(5);Sleep(200);
    }
    PostMessageW(main,WM_CLOSE,0,0);if(WaitForSingleObject(pi.hProcess,10000)==WAIT_TIMEOUT)TerminateProcess(pi.hProcess,4);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 0;
}
