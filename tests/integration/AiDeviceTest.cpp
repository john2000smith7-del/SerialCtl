#include "MainWindow.h"
#include "GatewayFixture.h"
#include "UiTestEvidence.h"
#include "Win32Helpers.h"
#include "resource.h"
#include <gdiplus.h>
#include <iostream>
#include <stdexcept>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <future>

namespace serialctl {
struct ApiServerTestAccess {
    static std::unique_lock<std::mutex> HoldPublication(ApiServer& server) {
        return std::unique_lock<std::mutex>(server.mutex_);
    }
};
namespace {
void Require(bool ok, const char* reason) {
    if (!ok) { std::cerr << "FAILED: " << reason << std::endl; throw std::runtime_error(reason); }
}
struct Transport : IConnection {
    bool connected = true;
    unsigned input = CP_UTF8;
    unsigned writes = 0;
    Bytes sent;
    bool Start(DataCallback, StatusCallback, std::wstring&) override { return true; }
    void Stop() override { connected = false; }
    bool IsConnected() const override { return connected; }
    bool Send(const Bytes& data, std::wstring&) override { ++writes; sent = data; return connected; }
    unsigned InputCodePage() const override { return input; }
    unsigned OutputCodePage() const override { return CP_UTF8; }
};
Json Wait(PowerService& power, Json action) {
    Require(!action.contains("error"), "Power operation not accepted");
    const auto end = GetTickCount64() + 5000;
    do {
        auto result = power.Action(action.at("id"));
        if (result.value("state", std::string()) != "queued" && result.value("state", std::string()) != "running")
            return result;
        Sleep(10);
    } while (GetTickCount64() < end);
    throw std::runtime_error("Power operation timed out");
}
}
struct AiDeviceTestAccess {
    static LRESULT CALLBACK FixtureParent(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        if(m==WM_DRAWITEM || m==WM_MEASUREITEM || m==WM_PAINT || (m>=WM_CTLCOLORMSGBOX && m<=WM_CTLCOLORSTATIC)) return reinterpret_cast<MainWindow*>(data)->HandleMessage(m,w,l);
        return DefSubclassProc(h,m,w,l);
    }
    static HWND PowerControl(HWND pane, int id) {
        struct Search { int id; HWND found; } search{id,nullptr};
        EnumChildWindows(pane, [](HWND child,LPARAM param)->BOOL {
            auto &s=*reinterpret_cast<Search*>(param);
            if(GetDlgCtrlID(child)==s.id) { s.found=child; return FALSE; } return TRUE;
        },reinterpret_cast<LPARAM>(&search));
        return search.found;
    }
    static void Run(HWND parent) {
        MainWindow main;
        main.measurePaint_ = true;
        struct WindowLifetime {
            HWND handle;
            ~WindowLifetime() {
                if (IsWindow(handle))
                    DestroyWindow(handle);
            }
        } windowLifetime{parent};
        main.window_ = parent;
        main.instance_ = GetModuleHandleW(nullptr);
        RECT size{0, 0, 1400, 850};
        AdjustWindowRectEx(&size, WS_OVERLAPPEDWINDOW, FALSE, 0);
        SetWindowPos(parent, nullptr, 0, 0, size.right - size.left, size.bottom - size.top, SWP_NOZORDER);
        RECT viewport{};
        SetWindowSubclass(parent,FixtureParent,9207,reinterpret_cast<DWORD_PTR>(&main));
        main.CreateControls();
        main.RefreshPowerTargets();
        Require(SendMessageW(main.powerMonitorTarget_,CB_GETCOUNT,0,0)==1 &&
            SendMessageW(main.powerMonitorTarget_,CB_GETCURSEL,0,0)==0,
            "Power terminal selector must show its placeholder before any session is connected");
        SendMessageW(main.commandList_,LB_SETITEMHEIGHT,0,66);
        GetClientRect(parent, &viewport);
        LONG previousNavigationRight = 0;
        for (HWND button : main.toolbarButtons_) {
            RECT bounds{};
            GetWindowRect(button, &bounds);
            MapWindowPoints(nullptr, parent, reinterpret_cast<POINT *>(&bounds), 2);
            Require(bounds.left > previousNavigationRight && bounds.top > 0 &&
                        bounds.right <= viewport.right && bounds.bottom <= viewport.bottom &&
                        bounds.bottom - bounds.top > 24,
                    "Navigation retains overlapping creation bounds before the main window is shown");
            previousNavigationRight = bounds.right;
        }
        ShowWindow(parent, SW_SHOW);
        GetClientRect(parent, &viewport);
        auto rightActionsInside = [&] {
            for (HWND action : {main.themeButton_, main.rightPanelToggleButton_}) {
                RECT bounds{};
                GetWindowRect(action, &bounds);
                MapWindowPoints(nullptr, parent, reinterpret_cast<POINT *>(&bounds), 2);
                Require(bounds.left >= 0 && bounds.right <= viewport.right && bounds.top >= 0 &&
                            bounds.bottom <= viewport.bottom,
                        "Right-edge actions escaped the current client after resize");
            }
        };
        main.windowResizing_ = true;
        main.HandleMessage(WM_SIZE, SIZE_RESTORED, MAKELPARAM(viewport.right + 80, viewport.bottom + 40));
        rightActionsInside();
        // Simulate an earlier layout completing after the native border moved.
        main.LayoutControls(viewport.right + 80, viewport.bottom + 40);
        main.HandleMessage(WM_EXITSIZEMOVE, 0, 0);
        Require(!main.windowResizing_, "Resize completion did not release deferred terminal reflow");
        rightActionsInside();
        main.pendingMode_ = 3;
        const auto remote = CreateDialogParamW(main.instance_, MAKEINTRESOURCEW(IDD_CONNECTION), parent,
                                               MainWindow::ConnectionDialogProc, reinterpret_cast<LPARAM>(&main));
        Require(remote != nullptr, "Remote connection dialog missing");
        RECT dialogBounds{};
        GetClientRect(remote, &dialogBounds);
        for (int id : {IDC_HOST, IDC_SERIAL_REFRESH, IDC_SERIAL, IDC_DIALOG_ERROR, IDCANCEL, IDOK}) {
            RECT control{};
            GetWindowRect(GetDlgItem(remote, id), &control);
            MapWindowPoints(nullptr, remote, reinterpret_cast<POINT *>(&control), 2);
            Require(control.left >= 0 && control.top >= 0 && control.right <= dialogBounds.right &&
                        control.bottom <= dialogBounds.bottom,
                    "Remote discovery controls escape compact dialog");
        }
        unsigned visibleSelectors = 0;
        for (HWND child = GetWindow(remote, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t className[32]{};
            GetClassNameW(child, className, 32);
            if (_wcsicmp(className, L"ComboBox") == 0 && (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE))
                ++visibleSelectors;
        }
        Require(visibleSelectors == 1, "Remote dialog must offer only the discovered SerialCtl port selector");
        for (int id : {IDC_SERIAL}) {
            RECT field{};
            GetWindowRect(GetDlgItem(remote, id), &field);
            Require(field.bottom - field.top == 28, "Remote dropdown must retain the 28 px field height");
        }
        RECT hostField{}, remoteRefresh{};
        auto hostFrame = reinterpret_cast<HWND>(GetPropW(GetDlgItem(remote, IDC_HOST), L"SerialCtl.FieldFrame"));
        Require(hostFrame != nullptr, "Remote IP field frame missing");
        GetWindowRect(hostFrame, &hostField);
        GetWindowRect(GetDlgItem(remote, IDC_SERIAL_REFRESH), &remoteRefresh);
        std::cout << "[REMOTE GEOMETRY] IP=" << hostField.left << ',' << hostField.top << ',' << hostField.right << ','
                  << hostField.bottom << " refresh=" << remoteRefresh.left << ',' << remoteRefresh.top << ','
                  << remoteRefresh.right << ',' << remoteRefresh.bottom << '\n';
        Require(hostField.bottom - hostField.top == 28 && remoteRefresh.top == hostField.top &&
                    remoteRefresh.bottom == hostField.bottom && remoteRefresh.left > hostField.right,
                "Remote Refresh must align with IP field, never overlap the device dropdown");
        ShowWindow(remote, SW_SHOW);
        for (const bool dark : {false, true}) {
            main.darkMode_ = dark;
            main.ApplyTheme();
            ui_test::SaveUiEvidence(remote, dark ? L"v205-remote-dark.png" : L"v205-remote-light.png");
        }
        main.darkMode_ = false;
        main.ApplyTheme();
        {
            test::Wsa sockets;
            test::Fixture gateway(18540, 18555);
            main.discoveryExplicitPort_ = static_cast<std::uint16_t>(gateway.server.Port());
            auto query = [&] {
                main.DiscoverSharedSerialPorts(remote);
                if (main.discoveryThread_.joinable()) main.discoveryThread_.join();
                MSG notification{};
                while (PeekMessageW(&notification, parent, WM_APP, WM_APP + 100, PM_REMOVE))
                    main.HandleMessage(notification.message, notification.wParam, notification.lParam);
            };
            query();
            Require(IsWindowEnabled(GetDlgItem(remote, IDOK)) && !main.pendingRemoteInstance_.empty() &&
                        SendDlgItemMessageW(remote, IDC_SERIAL, CB_GETCOUNT, 0, 0) == 2,
                    "Native remote dialog failed to discover the controlled SerialCtl COM3/COM5 service");
            std::wstring error;
            Require(main.ReadConnectionDialog(remote, error) && main.pendingRemoteSerial_ == L"COM3",
                    "Native remote dialog did not pin the discovered SerialCtl port");
            gateway.server.Stop();
            query();
            Require(!IsWindowEnabled(GetDlgItem(remote, IDOK)) && main.pendingRemoteInstance_.empty() &&
                        MainWindow::ControlText(GetDlgItem(remote, IDC_SERIAL)) == L"未发现可连接的串口" &&
                        !main.ReadConnectionDialog(remote, error),
                    "Missing SerialCtl service fabricated a connectable UDP fallback");
        }
        main.discoveryCancel_ = true;
        main.discoveryDialog_ = nullptr;
        DestroyWindow(remote);
        main.pendingMode_ = 1;
        const auto local = CreateDialogParamW(main.instance_, MAKEINTRESOURCEW(IDD_CONNECTION), parent,
                                              MainWindow::ConnectionDialogProc, reinterpret_cast<LPARAM>(&main));
        Require(local != nullptr, "Local serial dialog missing");
        for (const auto dialog : {local}) {
            RECT bounds{}, refresh{}, selector{};
            GetClientRect(dialog, &bounds);
            GetWindowRect(GetDlgItem(dialog, IDC_SERIAL_REFRESH), &refresh);
            GetWindowRect(GetDlgItem(dialog, IDC_SERIAL), &selector);
            Require(refresh.top == selector.top && refresh.bottom == selector.bottom,
                    "Serial Refresh must match final dropdown height");
            Require(selector.right - selector.left > refresh.right - refresh.left + 180,
                    "Hardware selector is still squeezed by Refresh");
            for (const int id : {IDC_SERIAL, IDC_BAUD, IDC_DATABITS, IDC_PARITY, IDC_STOPBITS, IDC_FLOW}) {
                RECT field{};
                GetWindowRect(GetDlgItem(dialog, id), &field);
                Require(field.bottom - field.top == 28, "Collapsed serial dropdown must be 28 px at test DPI");
                MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT *>(&field), 2);
                Require(field.bottom <= bounds.bottom, "Serial field escapes resized dialog");
            }
        }
        DestroyWindow(local);
        // Exercise actual production owner painters, preserving row/page selection separately from a held press.
        auto render = [&](HWND button, UINT state, bool power) {
            RECT rect{};
            GetClientRect(button, &rect);
            // Inactive power pages have not received layout yet. Exercise their
            // real painter at a normal selector size; layout is checked separately.
            if (power && (rect.right < 32 || rect.bottom < 28))
                rect = {0, 0, 128, 32};
            HDC screen = GetDC(button), dc = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right, rect.bottom);
            auto old = SelectObject(dc, bitmap);
            DRAWITEMSTRUCT item{};
            item.CtlType = ODT_BUTTON;
            item.CtlID = GetDlgCtrlID(button);
            item.hwndItem = button;
            item.hDC = dc;
            item.rcItem = rect;
            item.itemState = state;
            if (power)
                SendMessageW(main.powerPane_.Handle(), WM_DRAWITEM, item.CtlID, reinterpret_cast<LPARAM>(&item));
            else
                main.DrawOwnerItem(item);
            const auto pixel = GetPixel(dc, 8, rect.bottom / 2);
            SelectObject(dc, old);
            DeleteObject(bitmap);
            DeleteDC(dc);
            ReleaseDC(button, screen);
            return pixel;
        };
        for (const bool dark : {false, true}) {
            main.darkMode_ = dark;
            main.ApplyTheme();
            main.aiVisible_ = main.powerVisible_ = true;
            for (const auto button : main.toolbarButtons_) {
                const auto normal = render(button, 0, false);
                Require(normal != RGB(31, 79, 216), "Current navigation page latched accent fill");
                Require(render(button, ODS_SELECTED, false) == RGB(31, 79, 216),
                        "Navigation press must be solid deep blue");
                Require(render(button, ODS_FOCUS | ODS_NOFOCUSRECT, false) == normal,
                        "Mouse focus must restore neutral navigation");
            }
            for (int id : {90, 91, 110, 190}) {
                HWND button = PowerControl(main.powerPane_.Handle(), id);
                const auto normal = render(button, 0, true);
                Require(normal != RGB(31, 79, 216), "Power selector latched accent fill");
                const auto down = render(button, ODS_SELECTED, true);
                Require(down == RGB(31, 79, 216), "Power selector press differs from toolbar");
                Require(render(button, ODS_FOCUS | ODS_NOFOCUSRECT, true) == normal,
                        "Power selector mouse focus retained press color");
            }
        }
        const auto commandDraft = main.commands_;
        main.commands_ = {CommandItem(L"Controlled command", L"echo fixture")};
        MoveWindow(main.commandList_, 0, 0, 280, 200, TRUE);
        main.RefreshCommandList();
        for (const bool dark : {false, true}) {
            main.darkMode_ = dark;
            const auto c = UiTheme(dark);
            HDC screen = GetDC(parent), dc = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, 280, 66);
            const auto old = SelectObject(dc, bitmap);
            DRAWITEMSTRUCT item{};
            item.CtlType = ODT_LISTBOX;
            item.CtlID = GetDlgCtrlID(main.commandList_);
            item.itemID = 0;
            item.itemState = ODS_SELECTED;
            item.hwndItem = main.commandList_;
            item.hDC = dc;
            item.rcItem = {0, 0, 280, 66};
            main.DrawOwnerItem(item);
            Require(GetPixel(dc, 5, 33) == c.raised, "Selected command retained a sticky blue card");
            main.pressedCommandIndex_ = 0;
            main.pressedCommandAction_ = 2;
            main.DrawOwnerItem(item);
            Require(GetPixel(dc, 244, 33) == c.accent, "Command action has no deep-blue held feedback");
            SendMessageW(main.commandList_, WM_LBUTTONUP, 0, 0);
            Require(main.pressedCommandIndex_ == -1 && main.pressedCommandAction_ == 0,
                    "Command press did not reset after mouse release");
            main.DrawOwnerItem(item);
            Require(GetPixel(dc, 244, 33) == c.raised, "Command action retained blue after release");
            SelectObject(dc, old);
            DeleteObject(bitmap);
            DeleteDC(dc);
            ReleaseDC(parent, screen);
        }
        main.commands_ = commandDraft;
        main.RefreshCommandList();
        main.aiVisible_ = main.powerVisible_ = false;
        main.darkMode_ = false;
        main.ApplyTheme();
        main.pendingMode_ = 0;
        main.rightPanelWidth_ = 320;
        main.rightPanelCollapsed_ = false;
        std::vector<std::shared_ptr<Transport>> transports;
        for (int mode = 0; mode <= 4; ++mode) {
            auto session = std::make_unique<MainWindow::SessionState>();
            session->id = mode + 1;
            session->mode = mode;
            session->name = L"offline fixture " + std::to_wstring(mode);
            session->lineEndingIndex = mode == 0 ? 1 : 0;
            auto transport = std::make_shared<Transport>();
            if (mode == 4) transport->input = 936;
            session->connection = transport;
            if (mode == 1 || mode == 4)
                main.sessionService_.Register({{"id", "session-" + std::to_string(mode + 1)},
                    {"kind", mode == 4 ? "cmd" : "serial"}}, transport);
            transports.push_back(transport);
            main.sessions_.push_back(std::move(session));
        }
        main.sessions_[3]->udpRaw = true;
        main.sessions_[3]->udpReceived = true;
        const auto udpMetadata = main.AiDeviceRequest("sessions", Json::object());
        Require(udpMetadata[3]["dataReceived"] == true && udpMetadata[3]["peerVerified"] == false,
                "Raw UDP data was mistaken for verified device identity");
        main.sessions_[3]->udpRaw = false;
        const auto sessions = main.AiDeviceRequest("sessions", Json::object());
        Require(sessions.size() == 5, "AI target list omitted an open terminal type");
        for (int mode = 0; mode <= 4; ++mode) {
            const auto id = "session-" + std::to_string(mode + 1);
            Require(sessions[mode]["kind"] == ai::TerminalKind(mode), "Wrong terminal kind");
            const std::string output = "ACTUAL_OUTPUT_" + std::to_string(mode) + "\n";
            main.AppendData(mode + 1, Bytes(output.begin(), output.end()), CP_UTF8, 0);
            const auto read = main.AiDeviceRequest("read", {{"sessionId", id}, {"lines", 1}});
            Require(read.at("text") == output && read.at("cursor") == 1, "Actual terminal receive was not retained for AI");
            Require(read.at("kind") == ai::TerminalKind(mode), "Read lost terminal kind");
            const std::string command = mode == 4 ? u8"echo 中文" : "echo evidence";
            const auto result = main.AiDeviceRequest("command", {{"sessionId", id}, {"command", command}});
            Require(!result.contains("error") && result["execution"] == "unknown", "Terminal submission incorrectly failed or claimed execution");
            const std::wstring ending = mode == 4 ? L"\r\n" : L"\r";
            auto expected = WideToMultiByte((mode == 4 ? L"echo 中文" : L"echo evidence") + ending, transports[mode]->input);
            Require(transports[mode]->sent == Bytes(expected.begin(), expected.end()), "AI wrote wrong target, encoding or line ending");
        }
        {
            const auto sshId="session-1";
            auto &ssh=*main.sessions_[0];
            ssh.terminal.Feed(L"\x1b[?1049h\x1b[?2004h\x1b[>1u\x1b[2;1H真实 TUI 内容",L"");
            auto text=main.AiDeviceRequest("text",{{"sessionId",sshId},{"text","hello"}});
            Require(!text.contains("error") && transports[0]->sent==Bytes({'\x1b','[','2','0','0','~','h','e','l','l','o','\x1b','[','2','0','1','~'}),"TUI text must bracket paste without auto Enter");
            auto enter=main.AiDeviceRequest("key",{{"sessionId",sshId},{"key","Enter"}});
            const std::string enhanced="\x1b[13u";
            Require(!enter.contains("error") && transports[0]->sent==Bytes(enhanced.begin(),enhanced.end()),"AI real Enter must honor negotiated keyboard mode");
            const auto screen=main.AiDeviceRequest("screen",{{"sessionId",sshId}});
            Require(screen["alternateScreen"]==true && screen["text"].get<std::string>().find(u8"真实 TUI 内容")!=std::string::npos,"AI must read rendered VT screen, not raw ANSI logs");
            Require(main.AiDeviceRequest("text",{{"sessionId",sshId},{"text",std::string(1,3)}}).contains("error"),"Text path must not accept disguised control keys");
            Require(main.AiDeviceRequest("key",{{"sessionId","session-5"},{"key","Enter"}}).contains("error"),"CMD line submission must retain its separate semantics");
            ssh.terminal.Feed(L"\x1b[?1049l",L"");
        }
        main.SwitchSession(4, MainWindow::SessionFocus::Preserve);
        {
            const auto writes=transports[4]->writes;
            main.commands_={CommandItem(L"A",L"echo A"),CommandItem(L"B",L"echo B"),CommandItem(L"C",L"echo C")};
            main.commandsDirty_=false; main.RefreshCommandList();
            RECT row{}; SendMessageW(main.commandList_,LB_GETITEMRECT,0,reinterpret_cast<LPARAM>(&row));
            const int h=row.bottom-row.top;
            // Start in the text area (not just the small left-hand icon), and
            // require actual pointer movement before a drag preview appears.
            const int bodyX = std::min<int>(row.right - 90, row.left + 60);
            auto begin=MAKELPARAM(bodyX,h/2), moved=MAKELPARAM(bodyX+25,h*2+h/2);
            SendMessageW(main.commandList_,WM_LBUTTONDOWN,MK_LBUTTON,begin);
            Require(!main.commandDragGhost_ && !main.commandsDirty_,"Pressing a card without moving must not start a drag");
            if (GetSystemMetrics(SM_CXDRAG) / 2 > 1 && GetSystemMetrics(SM_CYDRAG) / 2 > 1) {
                SendMessageW(main.commandList_,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bodyX+1,h/2+1));
                Require(!main.commandDragGhost_,"Sub-threshold pointer movement must remain a click");
            }
            SendMessageW(main.commandList_,WM_LBUTTONUP,0,begin);
            Require(!main.commandDragGhost_ && !main.commandsDirty_,"Releasing a click must not reorder commands");
            SendMessageW(main.commandList_,WM_LBUTTONDOWN,MK_LBUTTON,begin);
            SendMessageW(main.commandList_,WM_MOUSEMOVE,MK_LBUTTON,moved);
            Require(main.commandDragGhost_ && main.commands_[0].name==L"A","Dragging card text must create a preview without committing order");
            RECT ghost{}; GetWindowRect(main.commandDragGhost_,&ghost); MapWindowPoints(nullptr,main.commandList_,reinterpret_cast<POINT*>(&ghost),2);
            Require(ghost.top==h*2 && ghost.left==25,"Drag preview must track pointer in both dimensions");
            ui_test::SaveUiEvidence(main.commandList_,L"v207-command-drag-held.png");
            SendMessageW(main.commandList_,WM_KEYDOWN,VK_ESCAPE,0);
            Require(!main.commandDragGhost_ && main.commands_[0].name==L"A" && !main.commandsDirty_,"Esc must restore original command order");
            SendMessageW(main.commandList_,WM_LBUTTONDOWN,MK_LBUTTON,begin);
            SendMessageW(main.commandList_,WM_MOUSEMOVE,MK_LBUTTON,moved);
            SendMessageW(main.commandList_,WM_LBUTTONUP,0,moved);
            Require(main.commands_[2].name==L"A" && transports[4]->writes==writes,"Only release commits reorder; drag never executes");
            main.commandsDirty_=false;
            SendMessageW(main.commandList_,WM_LBUTTONDOWN,MK_LBUTTON,begin);
            SendMessageW(main.commandList_,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(-24,h+h/2));
            SendMessageW(main.commandList_,WM_LBUTTONUP,0,MAKELPARAM(-24,h+h/2));
            Require(!main.commandDragGhost_ && !main.commandsDirty_ && main.commands_[0].name==L"B" && main.commands_[2].name==L"A",
                "Releasing outside the list must cancel reorder");
            const auto file=std::filesystem::temp_directory_path()/(L"serialctl-drop-"+std::to_wstring(GetCurrentProcessId())+L".txt");
            {std::ofstream stream(file);stream<<"echo imported-only\n";}
            main.commandsDirty_=false;
            const auto name=file.wstring();
            auto handle=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,sizeof(DROPFILES)+(name.size()+2)*sizeof(wchar_t));
            auto data=static_cast<DROPFILES*>(GlobalLock(handle)); data->pFiles=sizeof(DROPFILES);data->fWide=TRUE;
            memcpy(reinterpret_cast<char*>(data)+sizeof(DROPFILES),name.c_str(),(name.size()+1)*sizeof(wchar_t)); GlobalUnlock(handle);
            SendMessageW(main.commandList_,WM_DROPFILES,reinterpret_cast<WPARAM>(handle),0);
            Require(main.commands_.size()==1 && main.commands_[0].commands[0]==L"echo imported-only" && transports[4]->writes==writes,
                "Dropped UTF-8 file must use import without executing device input");
            std::filesystem::remove(file); main.commandsDirty_=false;
        }
        main.commands_ = {CommandItem(L"click regression", L"echo once")};
        main.RefreshCommandList();
        RECT actionRow{};
        SendMessageW(main.commandList_, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&actionRow));
        // Shared card metrics: the Run icon precedes Edit by one 12 px gap.
        const auto runPoint = MAKELPARAM(actionRow.right - 12 * 2 - 28 * 3 / 2,
                                        (actionRow.top + actionRow.bottom) / 2);
        const auto writes = transports[4]->writes;
        SendMessageW(main.commandList_, WM_LBUTTONDOWN, MK_LBUTTON, runPoint);
        Require(transports[4]->writes == writes, "Run submitted on mouse-down");
        SendMessageW(main.commandList_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-10, -10));
        SendMessageW(main.commandList_, WM_LBUTTONUP, 0, MAKELPARAM(-10, -10));
        Require(transports[4]->writes == writes, "Dragging out of Run failed to cancel");
        // Full-window GDI rendering can exceed the OS double-click interval on
        // a hosted desktop. Control event time rather than weakening the guard.
        ULONGLONG clickTime = GetTickCount64();
        const auto realClickStart = clickTime;
        main.commandClock_ = [&] { return clickTime; };
        for (int i = 0; i < 5; ++i) {
            ++clickTime;
            SendMessageW(main.commandList_, i == 1 ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, MK_LBUTTON, runPoint);
            SendMessageW(main.commandList_, WM_LBUTTONUP, 0, runPoint);
        }
        main.commandClock_ = {};
        std::cout << "[CLICK TIMING] Five input activations over 5 controlled milliseconds; native render wall time="
                  << GetTickCount64()-realClickStart << "ms, guard=" << std::max<DWORD>(350,GetDoubleClickTime())
                  << "ms, submitted=" << transports[4]->writes-writes << '\n';
        Require(transports[4]->writes == writes + 1, "Rapid Run clicks submitted duplicate commands");
        main.lastCommandActivation_ = 0;
        main.commands_[0].commands.push_back(L"echo should-not-run");
        main.commands_[0].intervalMs = 10000;
        main.SendCommand(0);
        Require(main.runningCommandIndex_ == 0, "Macro did not enter running state");
        SendMessageW(main.commandList_, WM_LBUTTONDOWN, MK_LBUTTON, runPoint);
        Require(main.runningCommandIndex_ == -1 && transports[4]->writes == writes + 2,
                "Immediate Stop was blocked by Run's repeat guard");
        SendMessageW(main.commandList_, WM_LBUTTONUP, 0, runPoint);
        main.SwitchSession(1, MainWindow::SessionFocus::Preserve);
        main.SwitchSession(4, MainWindow::SessionFocus::Preserve);
        Require(main.statusText_.find(L"已停止后续提交") != std::wstring::npos,
                "Session switch lost the macro stop reason");
        main.PostStatus(2, L"fixture background disconnect reason", true);
        MSG status{};
        Require(PeekMessageW(&status, parent, WM_APP + 2, WM_APP + 2, PM_REMOVE) != FALSE,
                "Connection status was not queued");
        main.HandleMessage(status.message, status.wParam, status.lParam);
        Require(main.sessions_[1]->unreadStatus && main.sessions_[1]->lastStatusIsError,
                "Background failure was not marked unread");
        main.SwitchSession(1, MainWindow::SessionFocus::Preserve);
        Require(!main.sessions_[1]->unreadStatus &&
                    main.statusText_.find(L"fixture background disconnect reason") != std::wstring::npos,
                "Selecting background session did not restore its failure reason");
        transports[0]->connected = false;
        main.SwitchSession(0, MainWindow::SessionFocus::Preserve);
        Require(!main.sftpThread_.joinable(), "Selecting disconnected SSH started a redundant SFTP process");
        transports[0]->connected = true;
        main.commands_ = commandDraft;
        main.RefreshCommandList();
        // Reuse the real terminal HWND and session model, not a copied AI log.
        main.ShowAiPage(false);
        main.aiPane_.SelectTarget("session-2");
        main.SyncAiTerminal();
        Require(main.aiVisible_ && main.activeSession_ == main.sessions_[1].get(),
            "AI target binding left the conversation page or selected wrong terminal");
        main.LayoutControls(viewport.right, viewport.bottom);
        Require(!main.aiMonitorCollapsed_, "AI monitor must start expanded");
        main.aiMonitorCollapsed_ = true;
        main.LayoutControls(viewport.right, viewport.bottom);
        Require(!IsWindowVisible(main.terminal_), "Collapsed AI monitor exposed terminal");
        main.aiMonitorCollapsed_ = false;
        main.aiMonitorRatio_ = 500;
        main.LayoutControls(viewport.right, viewport.bottom);
        RECT ai{}, terminal{}, header{};
        GetWindowRect(main.aiPane_.Handle(), &ai);
        GetWindowRect(main.terminal_, &terminal);
        GetWindowRect(main.aiTerminalHeader_, &header);
        std::cout << "[GEOMETRY] client=" << viewport.right << "x" << viewport.bottom << " chat=" << ai.right - ai.left
                  << " terminal=" << terminal.right - terminal.left << '\n';
        Require(std::abs((terminal.right - terminal.left) - (ai.right - ai.left)) <= 1,
                "Default AI and terminal widths must be equal");
        Require(!main.sessions_[1]->terminal.LineWrapView(), "AI monitor defaults to long-line projection");
        Require(IsWindowVisible(main.terminal_) && terminal.left > ai.right && terminal.top >= header.bottom,
            "Expanded monitor did not place actual terminal beside chat under its header");
        Require(main.rightPanelWidth_ == 320 && !main.rightPanelCollapsed_,
            "AI monitor changed terminal sidebar preference");
        Require(IsWindowVisible(main.aiPet_.Handle()), "AI desktop companion is not visible");
        RECT dock{}, connectionRows{}, footer{};
        GetWindowRect(main.aiPet_.Handle(), &dock); GetWindowRect(main.connectionList_, &connectionRows);
        GetWindowRect(main.disconnectAllButton_, &footer);
        Require(!(GetWindowLongPtrW(main.aiPet_.Handle(), GWL_STYLE) & WS_CHILD) &&
                GetWindow(main.aiPet_.Handle(), GW_OWNER) == nullptr,
                "Desktop companion must be unowned so main-window minimization cannot hide it");
        Require(connectionRows.bottom + 8 <= footer.top, "Desktop companion still reserves connection-list space");
        for (const bool dark : {false, true}) {
            main.darkMode_ = dark; main.ApplyTheme();
            ui_test::SaveUiEvidence(parent, dark ? L"v208-ai-pet-main-dark.png" : L"v208-ai-pet-main-light.png");
        }
        main.LayoutControls(viewport.right, 420);
        Require(IsWindowVisible(main.aiPet_.Handle()), "Short main window hid the desktop companion");
        main.LayoutControls(viewport.right, viewport.bottom);
        Require(IsWindowVisible(main.aiPet_.Handle()), "Restored window did not restore pet dock");
        main.darkMode_ = true;
        main.ApplyTheme();
        for (HWND button : {main.aiWrapButton_, main.rightPanelToggleButton_}) {
            RECT rect{};
            GetClientRect(button, &rect);
            HDC screen = GetDC(button), buffer = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right, rect.bottom);
            auto old = SelectObject(buffer, bitmap);
            for (UINT state : {0u, static_cast<UINT>(ODS_FOCUS | ODS_SELECTED)}) {
                DRAWITEMSTRUCT item{};
                item.CtlType = ODT_BUTTON;
                item.CtlID = GetDlgCtrlID(button);
                item.hwndItem = button;
                item.hDC = buffer;
                item.rcItem = rect;
                item.itemState = state;
                main.DrawOwnerItem(item);
                Require(GetPixel(buffer, 0, 0) == UiTheme(true).field,
                        "AI monitor button exposed a rectangular sidebar-colored exterior");
            }
            SelectObject(buffer, old);
            DeleteObject(bitmap);
            DeleteDC(buffer);
            ReleaseDC(button, screen);
        }
        main.darkMode_ = false;
        main.ApplyTheme();
        main.aiMonitorRatio_ = 600;
        main.LayoutControls(viewport.right, viewport.bottom);
        RECT resized{}; GetWindowRect(main.terminal_, &resized);
        Require(resized.right - resized.left > terminal.right - terminal.left,
            "Monitor width change did not resize real terminal");
        main.aiPane_.SelectTarget("session-5"); main.SyncAiTerminal();
        Require(main.aiVisible_ && main.activeSession_ == main.sessions_[4].get(),
            "Changing AI terminal target failed to bind actual CMD session");
        const std::string live = "LIVE_MONITOR_EVIDENCE\n";
        main.AppendData(5, Bytes(live.begin(), live.end()), CP_UTF8, 0);
        Require(main.AiDeviceRequest("read", {{"sessionId", "session-5"}, {"lines", 1}})["text"] == live,
            "Monitor receive did not share the AI device data source");
        main.aiPane_.SelectTarget(""); main.SyncAiTerminal();
        Require(!IsWindowVisible(main.terminal_), "Missing AI target exposed another session output");
        main.SwitchSession(4, MainWindow::SessionFocus::Preserve);
        main.LayoutControls(viewport.right, viewport.bottom);
        GetWindowRect(main.terminal_, &terminal); GetWindowRect(main.aiPane_.Handle(), &ai);
        Require(!main.aiVisible_ && IsWindowVisible(main.terminal_) && terminal.left == ai.left &&
            main.rightPanelWidth_ == 320, "Leaving AI failed to restore original terminal layout");
        // A final WM_MOUSEMOVE can be coalesced while output is repainting.
        // The real terminal handler must finish at WM_LBUTTONUP's coordinates.
        main.activeSession_->terminal.Reset();
        main.activeSession_->terminal.Feed(L"COPY_HEAD  \r\nHARD_LINE", L"");
        InvalidateRect(main.terminal_, nullptr, FALSE);
        UpdateWindow(main.terminal_);
        const auto metric = [&](const wchar_t* name) {
            const int value = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(main.terminal_, name)));
            Require(value > 0, "Terminal selection test geometry missing");
            return value;
        };
        const int left = metric(L"SerialCtl.TextLeft"), top = metric(L"SerialCtl.TextTop");
        const int cell = metric(L"SerialCtl.CellWidth"), row = metric(L"SerialCtl.LineHeight");
        const auto point = [&](int column, int line) {
            return MAKELPARAM(left + column * cell + cell / 2, top + line * row + row / 2);
        };
        const auto copied = [&] {
            Require(OpenClipboard(main.terminal_) != FALSE, "Cannot inspect terminal release clipboard");
            const auto data = GetClipboardData(CF_UNICODETEXT);
            const auto text = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
            const std::wstring value = text ? text : L"";
            if (text) GlobalUnlock(data);
            CloseClipboard();
            return value;
        };
        for (const bool finalMoveDelivered : {false, true}) {
            SendMessageW(main.terminal_, WM_LBUTTONDOWN, MK_LBUTTON, point(0, 0));
            if (finalMoveDelivered)
                SendMessageW(main.terminal_, WM_MOUSEMOVE, MK_LBUTTON, point(7, 1));
            SendMessageW(main.terminal_, WM_LBUTTONUP, 0, point(8, 1));
            Require(main.TerminalSelectionText(false) == L"COPY_HEAD  \r\nHARD_LINE" &&
                        copied() == L"COPY_HEAD  \r\nHARD_LINE",
                    "Terminal mouse release omitted its final character after a coalesced move");
        }
        SendMessageW(main.terminal_, WM_LBUTTONDOWN, MK_LBUTTON, point(8, 1));
        SendMessageW(main.terminal_, WM_LBUTTONUP, 0, point(0, 0));
        Require(copied() == L"COPY_HEAD  \r\nHARD_LINE", "Reverse terminal release lost hard line breaks");
        SendMessageW(main.terminal_, WM_LBUTTONDOWN, MK_LBUTTON, point(0, 0));
        SendMessageW(main.terminal_, WM_LBUTTONUP, 0, point(0, 0));
        Require(copied() == L"COPY_HEAD  \r\nHARD_LINE", "Ordinary click replaced the selection clipboard");
        main.ClearTerminalSelection();
        const auto before = transports[4]->sent;
        Require(main.AiDeviceRequest("command", {{"sessionId", "session-5"}, {"command", "a\nb"}}).contains("error"), "Multiline command bypassed validation");
        Require(transports[4]->sent == before, "Rejected command partially wrote");
        transports[0]->connected = false;
        Require(main.AiDeviceRequest("sessions", {}).size() == 4, "Disconnected terminal still offered");
        Require(main.AiDeviceRequest("read", {{"sessionId", "session-1"}}).contains("error"), "Disconnected terminal read accepted");

        Require(main.powerPane_.Handle() != nullptr, "Cannot create real power pane");
        main.powerMonitorSession_="session-2";
        main.ShowPowerPage();
        Require(main.powerVisible_ && !main.aiVisible_ && IsWindowVisible(main.powerPane_.Handle()) &&
            IsWindowVisible(main.terminal_) && !IsWindowVisible(main.commandList_),"Power page must share the actual selected terminal and hide original sidebar");
        RECT powerRect{},liveRect{};
        GetWindowRect(main.powerPane_.Handle(),&powerRect);GetWindowRect(main.terminal_,&liveRect);
        Require(std::abs((powerRect.right-powerRect.left)-(liveRect.right-liveRect.left))<=1,"Power monitor must start at independent 50:50 ratio");
        for(bool dark : {false,true}) {
            main.darkMode_=dark; main.ApplyTheme();
            for(int tab : {90,91,92,93}) {
                SendMessageW(main.powerPane_.Handle(),WM_COMMAND,tab,0);
                ui_test::SaveUiEvidence(parent,L"v207-power-"+std::wstring(dark?L"dark":L"light")+L"-tab-"+std::to_wstring(tab)+L".png");
                const auto footer=PowerControl(main.powerPane_.Handle(),248);
                RECT control{};GetWindowRect(footer,&control);
                Require(control.top>=powerRect.top && control.bottom<=powerRect.bottom,"Power recovery footer escaped the page");
            }
        }
        main.powerMonitorCollapsed_=true;main.LayoutControls(viewport.right,viewport.bottom);
        Require(!IsWindowVisible(main.terminal_),"Collapsed power monitor must not leave terminal overlay");
        main.powerMonitorCollapsed_=false;main.LayoutControls(viewport.right,viewport.bottom);
        for (int i = 0; i < 3; ++i)
            SendMessageW(PowerControl(main.powerPane_.Handle(), 20 + i), BM_SETCHECK, i == 1 ? BST_UNCHECKED : BST_CHECKED, 0);
        Require(Wait(main.powerService_, main.powerService_.Submit({{"type", "connect"}, {"backend", "simulation"}}))["state"] == "completed", "Controlled power backend not connected");
        auto state = main.AiDeviceRequest("power.get", {});
        Require(state["connected"] == true && state["selectedChannels"] == Json::array({1,3}), "Power status/GUI channel selection not exposed");
        Require(!state.contains("logPath") && !state.contains("testWrites"), "Power response leaked unrelated state");
        Json args = {{"enabled", true}, {"channels", state["selectedChannels"]}, {"identity", state["identity"]},
            {"mode", state["mode"]}, {"requestId", "ai-device-fixture-on"}};
        auto on = main.AiDeviceRequest("power.output", args);
        auto confirmed = Wait(main.powerService_, on);
        Require(confirmed["state"] == "completed" && confirmed["channels"][0]["state"] == "confirmed", "Power ON missing actual readback");
        Require(main.AiDeviceRequest("power.action", {{"id", on["id"]}})["state"] == "completed", "Power action cannot be queried");
        auto channels = main.powerService_.State()["channels"];
        Require(channels[0]["output"] == true && channels[1]["output"] == false && channels[2]["output"] == true, "Power affected an unchecked channel");
        SendMessageW(PowerControl(main.powerPane_.Handle(), 20), BM_SETCHECK, BST_UNCHECKED, 0);
        Require(main.AiDeviceRequest("power.output", args).contains("error"), "Changed channel selection was silently redirected");
        SendMessageW(PowerControl(main.powerPane_.Handle(), 20), BM_SETCHECK, BST_CHECKED, 0);
        args["enabled"] = false; args["requestId"] = "ai-device-fixture-off";
        Require(Wait(main.powerService_, main.AiDeviceRequest("power.output", args))["state"] == "completed", "Power OFF failed");
        Require(main.powerService_.State()["channels"][0]["output"] == false && main.powerService_.State()["channels"][2]["output"] == false, "Power OFF readback incorrect");
        Require(main.AiDeviceRequest("power.parameters", {{"voltage", 30}}).contains("error"), "AI gained unrequested parameter operation");
        {
            main.SwitchSession(1, MainWindow::SessionFocus::Preserve);
            main.LayoutControls(viewport.right, viewport.bottom);
            Json samples = Json::array();
            for (int sample = 0; sample < 16; ++sample) {
                // Hold the REAL gateway publication lock: local reception and
                // actual terminal painting must finish before network fan-out.
                auto gate = ApiServerTestAccess::HoldPublication(main.apiServer_);
                const auto marker = "SERIALCTL_PAINT_" + std::string(3 - std::to_string(sample).size(), '0') + std::to_string(sample);
                auto receive = std::async(std::launch::async, [&main, marker] {
                    main.PostData(2, Bytes(marker.begin(), marker.end()), CP_UTF8);
                });
                struct ReleaseBeforeJoin { std::unique_lock<std::mutex>& gate; ~ReleaseBeforeJoin() { if (gate.owns_lock()) gate.unlock(); } } release{gate};
                MSG notification{};
                const auto deadline = GetTickCount64() + 1000;
                bool posted = false;
                while (!(posted = PeekMessageW(&notification, parent, WM_APP + 1, WM_APP + 1, PM_REMOVE) != FALSE) && GetTickCount64() < deadline)
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_POSTMESSAGE);
                Require(posted, "Local serial display waited for the gateway lock");
                main.HandleMessage(notification.message, notification.wParam, notification.lParam);
                Require(reinterpret_cast<INT_PTR>(GetPropW(main.terminal_, L"SerialCtl.PaintProbe")) == sample + 1,
                        "Received serial marker did not reach actual terminal painting before return");
                Require(receive.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                        "Network lock gate was not exercising blocked fan-out");
                samples.push_back(reinterpret_cast<INT_PTR>(GetPropW(main.terminal_, L"SerialCtl.PaintUsec")));
                gate.unlock(); receive.get();
            }
            main.activeSession_->terminal.Clear();
            for (int i = 0; i < 64; ++i) main.QueueReceived({2, Bytes(16384, 'x'), CP_UTF8, {}, 0});
            main.HandleMessage(WM_APP + 1, 0, 0);
            Require(main.received_.empty() || main.receiveContinuation_, "Visible serial backlog reintroduced a receive timer");
            const auto deadline = GetTickCount64() + 5000;
            while (!main.received_.empty() && GetTickCount64() < deadline) UpdateWindow(parent);
            Require(main.received_.empty(), "Paint-driven serial receive continuation stopped making progress");
            const Json evidence{{"callbackToPaintUsec", samples}, {"samples", 16}, {"gatewayLockHeldDuringPaint", true},
                                {"visibleBacklogScheduling", "paint opportunities without a receive timer"},
                                {"scope", "actual Win32 terminal painter and receive handoff with controlled serial bytes; no UART/driver latency claim"}};
            std::cout << evidence.dump() << '\n';
            wchar_t directory[32768]{};
            if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", directory, 32768))
                std::ofstream(std::filesystem::path(directory) / L"serial-realtime-display.json") << evidence.dump(2);
        }
        main.powerService_.Shutdown();
        main.aiPane_.Shutdown();
        DestroyWindow(parent);
        for (auto font : {main.uiFont_, main.smallFont_, main.titleFont_, main.terminalFont_}) DeleteObject(font);
    }
};
}
int main() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput startup; ULONG_PTR token{}; Gdiplus::GdiplusStartup(&token, &startup, nullptr);
    HWND parent = CreateWindowExW(0,L"STATIC",L"AI device callback fixture",WS_OVERLAPPEDWINDOW,0,0,1000,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    int result = 0;
    try { serialctl::AiDeviceTestAccess::Run(parent); std::cout << "[PASS] Actual desktop AI callback: five terminal types, received evidence, target/encoding/CRLF, adjustable live AI monitor and original sidebar, power ON/OFF readback and unchanged selection; controlled transports, no physical devices\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; result = 1; }
    DestroyWindow(parent); Gdiplus::GdiplusShutdown(token); return result;
}
