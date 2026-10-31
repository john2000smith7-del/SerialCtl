// Actual desktop clicks. First-paint completion is application GDI paint, not a DWM/display presentation timestamp.
// Wait beyond the system double-click interval: production intentionally coalesces rapid submits.
// Abort on a missed click/unclosed modal so later samples can never reuse an old dialog.
#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <vector>
#include <string>
#include "../../src/app/resource.h"

// Controlled loopback Telnet output exercises the normal receive/paint path;
// no physical devices, real server credentials or test hooks in the product.
struct Traffic {
    SOCKET listener=INVALID_SOCKET;
    std::atomic_bool stop{false};
    std::atomic_ullong bytes{0};
    std::thread thread;
    unsigned port=0;
    bool started=false;
    bool Start() {
        WSADATA data{}; if(WSAStartup(MAKEWORD(2,2),&data))return false; started=true;
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(listener==INVALID_SOCKET)return false;
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        // A fresh hosted runner uses the documented Telnet default. This
        // fixture measures popup responsiveness, not cross-process edit automation.
        address.sin_port=htons(23);
        if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener,1))return false;
        int length=sizeof(address);if(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length))return false;
        port=ntohs(address.sin_port);
        thread=std::thread([this] {
            SOCKET peer=INVALID_SOCKET;
            while(!stop) {
                fd_set readable;FD_ZERO(&readable);FD_SET(listener,&readable);timeval timeout{0,100000};
                if(select(0,&readable,nullptr,nullptr,&timeout)>0){peer=accept(listener,nullptr,nullptr);break;}
            }
            if(peer==INVALID_SOCKET)return;
            DWORD timeout=100;setsockopt(peer,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
            std::string batch;
            for(int row=0;row<32;++row)batch+="LOOPBACK fixture: continuous terminal output while opening a connection form.\r\n";
            while(!stop) {
                const int sent=send(peer,batch.data(),static_cast<int>(batch.size()),0);
                if(sent<=0)break;bytes+=static_cast<unsigned>(sent);Sleep(20);
            }
            closesocket(peer);
        });
        return true;
    }
    ~Traffic(){stop=true;if(thread.joinable())thread.join();if(listener!=INVALID_SOCKET)closesocket(listener);if(started)WSACleanup();}
};

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
    if(argc<2 || argc>5)return 2;
    const bool requirePaint=argc>=4 && wcscmp(argv[3],L"first-paint")==0;
    const bool traffic=argc==5 && wcscmp(argv[4],L"traffic")==0;
    SetEnvironmentVariableW(L"SERIALCTL_UI_METRICS",L"1");
    const bool dark=argc>=3 && wcscmp(argv[2],L"dark")==0;
    wchar_t cmd[4096]{}; swprintf(cmd,4096,L"\"%ls\"",argv[1]);
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,cmd,nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){printf("create_failed=%lu\n",GetLastError());return 3;}
    WaitForInputIdle(pi.hProcess,10000); HWND main=await(pi.dwProcessId,false,20000);
    if(!main){printf("main_missing\n");TerminateProcess(pi.hProcess,3);return 3;}
    SetWindowPos(main,nullptr,0,0,1200,800,SWP_NOZORDER);SetForegroundWindow(main);Sleep(400);
    if(dark){if(!click(GetDlgItem(main,104))){TerminateProcess(pi.hProcess,9);return 9;}Sleep(GetDoubleClickTime()+100);}
    Traffic source;
    if(traffic) {
        if(!source.Start() || !click(GetDlgItem(main,102))){TerminateProcess(pi.hProcess,10);return 10;}
        HWND setup=await(pi.dwProcessId,true,10000);if(!setup){TerminateProcess(pi.hProcess,11);return 11;}
        DWORD_PTR idle=0;
        if(!SendMessageTimeoutW(setup,WM_NULL,0,0,SMTO_ABORTIFHUNG,5000,&idle)){TerminateProcess(pi.hProcess,12);return 12;}
        // Configure the controlled fixture outside the measured interval. Do
        // not race the baseline modal's initial activation/composition with a
        // second desktop click; all measured toolbar activations remain mouse.
        Sleep(100);
        SetDlgItemTextW(setup,IDC_HOST,L"127.0.0.1");SetDlgItemTextW(setup,IDC_PORT,std::to_wstring(source.port).c_str());
        if(!SendMessageTimeoutW(setup,WM_COMMAND,IDOK,0,SMTO_ABORTIFHUNG,5000,&idle)){TerminateProcess(pi.hProcess,12);return 12;}
        const auto limit=GetTickCount64()+10000;
        while(GetTickCount64()<limit && (IsWindow(setup) || !source.bytes))Sleep(10);
        if(IsWindow(setup) || !source.bytes){
            wchar_t error[512]{};if(IsWindow(setup))GetDlgItemTextW(setup,IDC_DIALOG_ERROR,error,512);
            wprintf(L"traffic_setup_failed port=%u dialog_alive=%d bytes=%llu error=%ls\n",source.port,IsWindow(setup),source.bytes.load(),error);
            TerminateProcess(pi.hProcess,13);return 13;
        }
        Sleep(500);
    }
    struct Spec {const char *name; int id;}; const Spec all[]={{"ssh",100},{"serial",101},{"telnet",102},{"remote",103},{"ai",304},{"settings",303}};
    for(const auto &spec:all) {
       for(int rep=0;rep<(traffic?2:30);rep++) {
          HWND b=GetDlgItem(main,spec.id);SetForegroundWindow(main);Sleep(GetDoubleClickTime()+100);
          const auto paintBefore=reinterpret_cast<UINT_PTR>(GetPropW(GetDlgItem(main,107),L"SerialCtl.PaintSample"));
          const auto bytesBefore=source.bytes.load();
          ULONGLONG start=GetTickCount64();if(!click(b)){printf("%s,%d,click_failed\n",spec.name,rep);TerminateProcess(pi.hProcess,5);return 5;}ULONGLONG eventSent=GetTickCount64();
          HWND dialog=nullptr;ULONGLONG created=0,visible=0,caption=0,painted=0;const ULONGLONG deadline=start+12000;
          while(GetTickCount64()<deadline) {
            if(!dialog){dialog=find(pi.dwProcessId,true,false,false);if(dialog)created=GetTickCount64();}
            if(dialog&&!visible&&IsWindowVisible(dialog))visible=GetTickCount64();
            if(dialog&&visible){wchar_t title[256]{};GetWindowTextW(dialog,title,256);if(title[0]){if(!caption)caption=GetTickCount64();painted=reinterpret_cast<UINT_PTR>(GetPropW(dialog,L"SerialCtl.DialogFirstPaint"));if(!requirePaint || painted)break;}}
            Sleep(1);
          }
          if(!dialog || !visible || !caption || (requirePaint && !painted)){printf("%s,%d,timeout,created_ms=%llu\n",spec.name,rep,created?created-start:0);TerminateProcess(pi.hProcess,6);return 6;}
          DWORD_PTR result=0;BOOL ready=SendMessageTimeoutW(dialog,WM_NULL,0,0,SMTO_ABORTIFHUNG,5000,&result);ULONGLONG done=GetTickCount64();if(!ready){TerminateProcess(pi.hProcess,7);return 7;}
          const auto paintAfter=reinterpret_cast<UINT_PTR>(GetPropW(GetDlgItem(main,107),L"SerialCtl.PaintSample"));
          printf("%s,%d,event_ms=%llu,created_ms=%llu,visible_ms=%llu,caption_ms=%llu,responsive_ms=%llu,responsive=%d,paint_delta=%llu,first_paint_ms=%lld\n",spec.name,rep,eventSent-start,created-start,visible-start,caption-start,done-start,ready?1:0,paintAfter-paintBefore,painted?static_cast<long long>(painted-start):-1LL);fflush(stdout);
          if(traffic)printf("traffic,%s,%d,sent_before=%llu,sent_after=%llu\n",spec.name,rep,bytesBefore,source.bytes.load());
          PostMessageW(dialog,WM_CLOSE,0,0);ULONGLONG end=GetTickCount64()+5000;while(IsWindow(dialog)&&GetTickCount64()<end)Sleep(5);if(IsWindow(dialog)){TerminateProcess(pi.hProcess,8);return 8;}Sleep(200);
       }
    }
    PostMessageW(main,WM_CLOSE,0,0);
    if(WaitForSingleObject(pi.hProcess,10000)==WAIT_TIMEOUT){TerminateProcess(pi.hProcess,4);return 4;}
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 0;
}
