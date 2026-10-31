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
#include <chrono>

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
struct HeldTransport : IConnection {
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic_bool connected{true}, cancelled{false};
    ~HeldTransport() override { if (release) CloseHandle(release); if (entered) CloseHandle(entered); }
    bool Start(DataCallback, StatusCallback, std::wstring&) override { return true; }
    void CancelStart() override { cancelled = true; }
    void Stop() override {
        SetEvent(entered);
        WaitForSingleObject(release, INFINITE);
        connected = false;
    }
    bool IsConnected() const override { return connected; }
    bool Send(const Bytes&, std::wstring&) override { return connected; }
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
    static inline unsigned hostIdentityUpdates = 0;
    static LRESULT CALLBACK ObserveHostIdentity(int code, WPARAM w, LPARAM l) {
        if (code >= 0) {
            const auto& message = *reinterpret_cast<const CWPSTRUCT*>(l);
            if (message.message == WM_SETTEXT && GetDlgCtrlID(message.hwnd) == IDC_HOST_IDENTITY &&
                message.lParam && *reinterpret_cast<const wchar_t*>(message.lParam))
                ++hostIdentityUpdates;
        }
        return CallNextHookEx(nullptr, code, w, l);
    }
    static LRESULT CALLBACK FixtureParent(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        if(m==WM_DRAWITEM || m==WM_MEASUREITEM || m==WM_PAINT || m==WM_TIMER ||
           (m>=WM_APP && m<=WM_APP+100) || (m>=WM_CTLCOLORMSGBOX && m<=WM_CTLCOLORSTATIC))
            return reinterpret_cast<MainWindow*>(data)->HandleMessage(m,w,l);
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
        Require(MainWindow::ControlText(GetDlgItem(remote, IDC_PORT)) == L"7000",
                "Remote discovery lost its hidden default service port");
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
        Require(!main.serialPortScan_, "Serial enumeration started before the form was shown");
        Require(!IsWindowEnabled(GetDlgItem(local, IDOK)) &&
                    IsWindowEnabled(GetDlgItem(local, IDCANCEL)),
                "Pending serial lookup must allow Cancel and prevent an invalid connection");
        Require(MainWindow::ControlText(GetDlgItem(local, IDC_BAUD)) == L"115200" &&
                SendDlgItemMessageW(local, IDC_DATABITS, CB_GETCURSEL, 0, 0) == 3 &&
                SendDlgItemMessageW(local, IDC_PARITY, CB_GETCURSEL, 0, 0) == 0 &&
                SendDlgItemMessageW(local, IDC_STOPBITS, CB_GETCURSEL, 0, 0) == 0 &&
                SendDlgItemMessageW(local, IDC_FLOW, CB_GETCURSEL, 0, 0) == 0,
                "Local serial form lost its 115200/8N1/no-flow defaults");
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
        for (const bool dark : {false, true}) {
            main.darkMode_ = dark;
            main.ApplyTheme();
            ShowWindow(local, SW_SHOW);
            ui_test::SaveUiEvidence(local, dark ? L"dialog-serial-dark.png" : L"dialog-serial-light.png");
        }
        UpdateWindow(local);
        MSG firstPaintJob{};
        Require(PeekMessageW(&firstPaintJob, local, UiFirstPaintReady, UiFirstPaintReady, PM_REMOVE) != FALSE,
                "Local form did not schedule its scan after the first paint");
        DispatchMessageW(&firstPaintJob);
        Require(main.serialPortScan_ != nullptr, "Visible serial form did not start background enumeration");
        DestroyWindow(local);
        // Each connection form must keep its own usable defaults even when
        // setup work for the other modes is omitted. Never open a connection.
        for (const int mode : {0, 2}) {
            main.pendingMode_ = mode;
            hostIdentityUpdates = 0;
            struct Hook {
                HHOOK handle = SetWindowsHookExW(WH_CALLWNDPROC, ObserveHostIdentity, nullptr, GetCurrentThreadId());
                ~Hook() { if (handle) UnhookWindowsHookEx(handle); }
            } hook;
            Require(hook.handle != nullptr, "Cannot observe connection identity updates");
            const auto network = CreateDialogParamW(main.instance_, MAKEINTRESOURCEW(IDD_CONNECTION), parent,
                MainWindow::ConnectionDialogProc, reinterpret_cast<LPARAM>(&main));
            Require(network != nullptr, "Network connection dialog missing");
            Require(!main.initializingConnectionDialog_, "Connection initialization flag was not released");
            Require(hostIdentityUpdates == (mode == 0 ? 1u : 0u),
                    "Host/port defaults must produce only one final SSH identity refresh");
            Require(MainWindow::ControlText(GetDlgItem(network, IDC_HOST)) == L"127.0.0.1" &&
                    MainWindow::ControlText(GetDlgItem(network, IDC_PORT)) == (mode == 0 ? L"22" : L"23"),
                    "Connection mode lost its default host or port");
            Require(!IsWindowVisible(GetDlgItem(network, IDC_SERIAL)) &&
                    !IsWindowVisible(GetDlgItem(network, IDC_BAUD)),
                    "Network form exposes unrelated serial fields");
            std::wstring error;
            if (mode == 0) {
                // Initialization must suppress partial host/port queries;
                // edits made after initialization must still refresh status.
                main.initializingConnectionDialog_ = true;
                SetDlgItemTextW(network, IDC_HOST, L"serialctl-fixture.invalid");
                SetDlgItemTextW(network, IDC_PORT, L"2222");
                main.initializingConnectionDialog_ = false;
                Require(hostIdentityUpdates == 1, "Partial initialization refreshed host identity");
                SendMessageW(network, WM_COMMAND, MAKEWPARAM(IDC_HOST, EN_CHANGE),
                    reinterpret_cast<LPARAM>(GetDlgItem(network, IDC_HOST)));
                Require(hostIdentityUpdates == 2 &&
                        MainWindow::ControlText(GetDlgItem(network, IDC_HOST_IDENTITY)) == L"连接时核验主机身份",
                        "Live SSH host edits no longer refresh identity status");
                SetDlgItemTextW(network, IDC_PORT, L"0");
                Require(hostIdentityUpdates > 2 &&
                        MainWindow::ControlText(GetDlgItem(network, IDC_HOST_IDENTITY)) == L"连接时核验主机身份",
                        "Invalid live port lost its unverified identity status");
                SetDlgItemTextW(network, IDC_HOST, L"127.0.0.1");
                SetDlgItemTextW(network, IDC_PORT, L"22");
                Require(!main.ReadConnectionDialog(network, error), "SSH must still require a username");
                SetDlgItemTextW(network, IDC_USER, L"fixture");
                SetDlgItemTextW(network, IDC_PASSWORD, L"fixture-password");
            }
            Require(main.ReadConnectionDialog(network, error) && main.pendingPort_ == (mode == 0 ? 22 : 23),
                    "Network form cannot read its initialized settings");
            for (const bool dark : {false, true}) {
                main.darkMode_ = dark;
                main.ApplyTheme();
                ShowWindow(network, SW_SHOW);
                ui_test::SaveUiEvidence(network, mode == 0 ?
                    (dark ? L"dialog-ssh-dark.png" : L"dialog-ssh-light.png") :
                    (dark ? L"dialog-telnet-dark.png" : L"dialog-telnet-light.png"));
            }
            DestroyWindow(network);
        }
        main.darkMode_ = false;
        main.ApplyTheme();
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
        const auto commandDraft = main.macro_.Items();
        Require(main.notebook_.Editor() && main.notebook_.DoubleClick(), "Native notebook must offer default body double-click execution");
        main.aiVisible_ = main.powerVisible_ = false;
        main.darkMode_ = false;
        main.ApplyTheme();
        main.pendingMode_ = 0;
        main.rightPanelWidth_ = 320;
        main.rightPanelCollapsed_ = false;
        main.LayoutControls(1180,760);
        RECT leftBefore{},toggleBefore{},leftAfter{},toggleAfter{};
        GetWindowRect(main.terminal_,&leftBefore);GetWindowRect(main.connectionHeader_,&toggleBefore);
        main.SetLeftPanelCollapsed(true);
        const auto leftDeadline=GetTickCount64()+1000;
        while(main.leftPanelAnimating_ && GetTickCount64()<leftDeadline) {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);} Sleep(1);
        }
        GetWindowRect(main.terminal_,&leftAfter);GetWindowRect(main.connectionHeader_,&toggleAfter);
        Require(!main.leftPanelAnimating_ && !IsWindowVisible(main.connectionList_) && leftAfter.left<leftBefore.left && EqualRect(&toggleBefore,&toggleAfter),
                "Left rail did not free terminal space or moved its toggle during collapse");
        main.SetLeftPanelCollapsed(false);
        const auto restoreDeadline=GetTickCount64()+1000;
        while(main.leftPanelAnimating_ && GetTickCount64()<restoreDeadline) {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);} Sleep(1);
        }
        Require(IsWindowVisible(main.connectionList_),"Left rail did not restore the connection list");

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
            // A real drag/animation changes child bounds many times. Keep the
            // terminal grid stable until the operation finishes, then reflow.
            for (bool MainWindow::*deferred : {&MainWindow::windowResizing_, &MainWindow::rightPanelDragging_,
                                             &MainWindow::rightPanelAnimating_, &MainWindow::leftPanelAnimating_}) {
                RECT bounds{};
                GetWindowRect(main.terminal_, &bounds);
                const int oldColumns = main.terminalViewport_.Metrics().columns;
                const int oldRows = main.terminalViewport_.Metrics().rows;
                const int modelColumns = main.sessions_[4]->terminal.Columns();
                const int modelRows = main.sessions_[4]->terminal.Rows();
                main.*deferred = true;
                Require(SetWindowPos(main.terminal_, nullptr, 0, 0, bounds.right - bounds.left - 200,
                                     bounds.bottom - bounds.top - 60, SWP_NOMOVE | SWP_NOZORDER) != FALSE,
                        "Cannot resize terminal during deferred-reflow fixture");
                main.UpdateTerminalDimensions();
                Require(main.terminalViewport_.Metrics().columns == oldColumns &&
                            main.terminalViewport_.Metrics().rows == oldRows &&
                            main.sessions_[4]->terminal.Columns() == modelColumns &&
                            main.sessions_[4]->terminal.Rows() == modelRows,
                        "Terminal reflow must wait until drag, animation or resize completes");
                main.*deferred = false;
                main.UpdateTerminalDimensions();
                Require(main.terminalViewport_.Metrics().columns != oldColumns &&
                            main.sessions_[4]->terminal.Columns() == main.terminalViewport_.Metrics().columns,
                        "Terminal dimensions must be committed after deferred reflow");
                main.LayoutControls(viewport.right, viewport.bottom);
            }
        }
        {
            const auto originalText=main.notebook_.Text();
            main.notebook_.SetText(L"# 调试\r\necho first\r\necho second\r\n");
            const auto rows=CommandLines(main.notebook_.Text());
            Require(rows.size()==4 && main.notebook_.Text().find(L"echo first")!=std::wstring::npos,"RichEdit text and UTF16 line offsets must be readable");
            main.notebook_.Selection(static_cast<LONG>(rows[1].start+4),static_cast<LONG>(rows[2].start+4));
            Require(main.notebook_.SelectedCommands()==std::vector<std::wstring>{L"echo first",L"echo second"},"Native partial selection must extract complete commands");
            const auto writes=transports[4]->writes;
            main.notebook_.SetInterval(5000);
            main.RunNotebook(main.notebook_.SelectedCommands());
            Require(main.macro_.Running()&&transports[4]->writes==writes+1,"Notebook submits the first selected command immediately");
            main.notebook_.action(1);
            Require(!main.macro_.Running()&&transports[4]->writes==writes+1,"Same single Run action becomes Stop without sending the remaining command");
            Require(main.statusText_.find(L"已停止后续提交")!=std::wstring::npos,"Stop reports accepted count without claiming execution finished");
            std::wstring manyRows;for(int i=0;i<80;++i)manyRows+=L"echo row-"+std::to_wstring(i)+L"\r\n";
            main.notebook_.SetText(manyRows);main.notebook_.SelectLine(63);
            Require(main.notebook_.SelectedCommands()==std::vector<std::wstring>{L"echo row-63"},"Native paragraph offsets must map to the correct complete row after many CRLF lines");
            main.notebook_.SetText(std::wstring(300,L'A')+L"\r\necho end");
            Require(SendMessageW(main.notebook_.Editor(),EM_GETLINECOUNT,0,0)==2,"Long notebook commands must not soft-wrap");
            main.notebook_.Selection(200,200);SendMessageW(main.notebook_.Editor(),EM_SCROLLCARET,0,0);
            POINT scroll{};SendMessageW(main.notebook_.Editor(),EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
            Require(scroll.x>0,"Long notebook lines must scroll horizontally to reveal the caret");
            main.notebook_.SetText(L"echo ctrl-enter");main.notebook_.Selection(3,3);
            const auto beforeShortcut=main.notebook_.Text();const auto beforeWrites=transports[4]->writes;
            BYTE originalKeys[256]{},keys[256]{};GetKeyboardState(originalKeys);memcpy(keys,originalKeys,sizeof(keys));keys[VK_CONTROL]=0x80;SetKeyboardState(keys);
            SendMessageW(main.notebook_.Editor(),WM_KEYDOWN,VK_RETURN,0);
            SendMessageW(main.notebook_.Editor(),WM_CHAR,L'\n',0);SetKeyboardState(originalKeys);
            Require(transports[4]->writes==beforeWrites+1&&main.notebook_.Text()==beforeShortcut,"Ctrl+Enter sends the whole caret line without inserting a newline");
            main.notebook_.SetText(L"# 调试\r\necho first\r\necho second\r\n");
            const auto restoredRows=CommandLines(main.notebook_.Text());
            main.notebook_.Selection(static_cast<LONG>(restoredRows[1].start+4),static_cast<LONG>(restoredRows[2].start+4));
            main.notebook_.SetInterval(500);
            for(bool dark:{false,true}){
                main.darkMode_=dark;main.ApplyTheme();
                ui_test::SaveUiEvidence(main.notebook_.Handle(),dark?L"notebook-dark.png":L"notebook-light.png");
                Require(IsWindowVisible(main.notebook_.Editor()),"Theme changes keep the native editable document visible");
            }
            main.notebook_.Saved();
            const auto file=std::filesystem::temp_directory_path()/(L"serialctl-notebook-drop-"+std::to_wstring(GetCurrentProcessId())+L".txt");
            {std::ofstream stream(file);stream<<"# imported\necho imported-only\n";}
            const auto name=file.wstring();auto handle=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,sizeof(DROPFILES)+(name.size()+2)*sizeof(wchar_t));
            auto data=static_cast<DROPFILES*>(GlobalLock(handle));data->pFiles=sizeof(DROPFILES);data->fWide=TRUE;memcpy(reinterpret_cast<char*>(data)+sizeof(DROPFILES),name.c_str(),(name.size()+1)*sizeof(wchar_t));GlobalUnlock(handle);
            SendMessageW(main.notebook_.Editor(),WM_DROPFILES,reinterpret_cast<WPARAM>(handle),0);
            Require(main.notebook_.Text().find(L"echo imported-only")!=std::wstring::npos&&main.notebook_.Dirty()&&transports[4]->writes==beforeWrites+1,"Dropped UTF8 file updates a draft and never executes");
            std::filesystem::remove(file);
            main.notebook_.SetText(originalText);
            main.RunNotebook({L"echo retained-stop",L"echo must-not-be-sent"});
            main.notebook_.action(1);
        }
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
        Require(!main.sftp_.HasOperationThread(), "Selecting disconnected SSH started a redundant SFTP process");
        transports[0]->connected = true;
        main.macro_.Replace(commandDraft, false);
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
        // A normal page refresh now queues paint. Expose this fixture before
        // reading paint-produced geometry; UpdateWindow on an occluded child
        // alone need not paint its visible client region on a hosted desktop.
        SetWindowPos(parent, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(parent);
        RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        InvalidateRect(main.terminal_, nullptr, FALSE);
        UpdateWindow(main.terminal_);
        const auto metric = [&](const wchar_t* name) {
            const int value = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(main.terminal_, name)));
            if (value <= 0) std::wcerr << L"Missing metric: " << name << L" visible=" << IsWindowVisible(main.terminal_)
                << L" measure=" << main.measurePaint_ << L" cell=" << main.terminalViewport_.Metrics().cellWidth
                << L" row=" << main.terminalViewport_.Metrics().lineHeight << L'\n';
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
        Require(!IsWindowEnabled(PowerControl(main.powerPane_.Handle(), 103)),
                "Unavailable power sampling control must not accept changes that open rejection dialogs");
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
            // A paint-latency measurement needs an exposed native surface.
            // Earlier cases create an independent topmost desktop companion;
            // IsWindowVisible alone does not mean this fixture is unobscured.
            SetWindowPos(parent, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(parent);
            RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            // Exercise the application's ordinary message-loop dispatch, which
            // also delivers RDW_INTERNALPAINT without an invalid client region.
            unsigned dispatched = 0, paints = 0;
            const auto paintOpportunity = [&] {
                MSG next{};
                if (PeekMessageW(&next, nullptr, 0, 0, PM_REMOVE)) {
                    ++dispatched; if (next.message == WM_PAINT) ++paints;
                    TranslateMessage(&next); DispatchMessageW(&next);
                }
            };
            Json samples = Json::array();
            for (int sample = 0; sample < 16; ++sample) {
                // Hold the REAL gateway publication lock: sustained device
                // reception must return, and paint, before network fan-out.
                auto gate = ApiServerTestAccess::HoldPublication(main.apiServer_);
                const auto marker = "SERIALCTL_PAINT_" + std::string(3 - std::to_string(sample).size(), '0') + std::to_string(sample);
                auto receive = std::async(std::launch::async, [&main, marker] {
                    main.PostData(2, Bytes(marker.begin(), marker.end()), CP_UTF8);
                });
                struct ReleaseBeforeJoin { std::unique_lock<std::mutex>& gate; ~ReleaseBeforeJoin() { if (gate.owns_lock()) gate.unlock(); } } release{gate};
                Require(receive.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
                        "Device receive callback still waits for network publication");
                receive.get();
                MSG notification{};
                const auto deadline = GetTickCount64() + 1000;
                bool posted = false;
                while (!(posted = PeekMessageW(&notification, parent, WM_APP + 1, WM_APP + 1, PM_REMOVE) != FALSE) && GetTickCount64() < deadline)
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_POSTMESSAGE);
                Require(posted, "Local serial display waited for the gateway lock");
                main.HandleMessage(notification.message, notification.wParam, notification.lParam);
                if (reinterpret_cast<INT_PTR>(GetPropW(main.terminal_, L"SerialCtl.PaintProbe")) != sample + 1) {
                    RECT client{}; GetClientRect(main.terminal_, &client);
                    RECT clip{}; HDC surface = GetDC(main.terminal_);
                    const int region = GetClipBox(surface, &clip); ReleaseDC(main.terminal_, surface);
                    std::lock_guard<std::mutex> lock(main.receivedMutex_);
                    std::cerr << "FAILED serial paint state: sample=" << sample
                              << " painted=" << reinterpret_cast<INT_PTR>(GetPropW(main.terminal_, L"SerialCtl.PaintProbe"))
                              << " decoded=" << main.activeSession_->paintProbe
                              << " pendingArrival=" << main.activeSession_->pendingProbeArrival
                              << " active=" << main.activeSession_->id << " queued=" << main.received_.size()
                              << " visible=" << IsWindowVisible(main.terminal_) << " iconic=" << IsIconic(parent)
                              << " client=" << client.right << 'x' << client.bottom
                              << " clip=" << region << ':' << clip.left << ',' << clip.top << ',' << clip.right << ',' << clip.bottom
                              << " update=" << GetUpdateRect(main.terminal_, nullptr, FALSE) << std::endl;
                }
                Require(reinterpret_cast<INT_PTR>(GetPropW(main.terminal_, L"SerialCtl.PaintProbe")) == sample + 1,
                        "Received serial marker did not reach actual terminal painting before return");
                samples.push_back(reinterpret_cast<INT_PTR>(GetPropW(main.terminal_, L"SerialCtl.PaintUsec")));
                gate.unlock();
            }
            for (int i = 0; i < 32; ++i) main.QueueReceived({3, Bytes(16384, 'b'), CP_UTF8, {}, 0});
            main.receiveFairness_ = 0;
            LARGE_INTEGER arrival{}; QueryPerformanceCounter(&arrival);
            const std::string visibleMarker = "SERIALCTL_PAINT_200";
            main.QueueReceived({2, Bytes(visibleMarker.begin(), visibleMarker.end()), CP_UTF8, {}, arrival.QuadPart});
            main.HandleMessage(WM_APP + 1, 0, 0);
            Require(reinterpret_cast<INT_PTR>(GetPropW(main.terminal_, L"SerialCtl.PaintProbe")) == 201,
                    "Background terminal backlog buried the visible serial echo");
            auto backlogDeadline = GetTickCount64() + 5000;
            while (!main.received_.empty() && GetTickCount64() < backlogDeadline) paintOpportunity();
            if (!main.received_.empty())
                std::cerr << "FAILED receive continuation state: remaining=" << main.received_.size()
                          << " continuation=" << main.receiveContinuation_ << " posted=" << main.receivePosted_
                          << " dispatched=" << dispatched << " paints=" << paints
                          << " update=" << GetUpdateRect(parent, nullptr, FALSE)
                          << " queueStatus=" << GetQueueStatus(QS_PAINT | QS_POSTMESSAGE) << std::endl;
            Require(main.received_.empty(), "Visible-session priority starved background terminal output");
            main.activeSession_->terminal.Clear();
            for (int i = 0; i < 64; ++i) main.QueueReceived({2, Bytes(16384, 'x'), CP_UTF8, {}, 0});
            main.HandleMessage(WM_APP + 1, 0, 0);
            Require(main.received_.empty() || main.receiveContinuation_, "Visible serial backlog reintroduced a receive timer");
            const auto deadline = GetTickCount64() + 5000;
            while (!main.received_.empty() && GetTickCount64() < deadline) paintOpportunity();
            Require(main.received_.empty(), "Paint-driven serial receive continuation stopped making progress");
            const Json evidence{{"callbackToPaintUsec", samples}, {"samples", 16}, {"gatewayLockHeldDuringPaint", true},
                                {"visibleBacklogScheduling", "paint opportunities without a receive timer"},
                                {"scope", "actual Win32 terminal painter and receive handoff with controlled serial bytes; no UART/driver latency claim"}};
            std::cout << evidence.dump() << '\n';
            wchar_t directory[32768]{};
            if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", directory, 32768))
                std::ofstream(std::filesystem::path(directory) / L"serial-realtime-display.json") << evidence.dump(2);
            auto pending = std::make_unique<MainWindow::SessionState>();
            pending->id = 99; pending->mode = 3; pending->name = L"startup replay fixture";
            pending->connection = std::make_shared<Transport>();
            pending->logger = std::make_shared<SessionLogger>();
            main.pendingSession_ = std::move(pending);
            main.connectionCancel_ = false;
            for (int i = 0; i < 64; ++i) main.pendingConnectionData_.push_back({99, Bytes(16384, 'y'), CP_UTF8, {}, 0});
            main.pendingConnectionBytes_ = 64 * 16384;
            const std::string live = "LIVE_AFTER_HISTORY";
            main.QueueReceived({99, Bytes(live.begin(), live.end()), CP_UTF8, {}, 0});
            main.CompleteConnection(true, L"");
            Require(!main.received_.empty() && main.activeSession_->terminal.HistorySize() == 0,
                    "Connection completion synchronously parsed the entire startup history");
            Require(main.received_.front().data.front() == 'y' && main.received_.back().data == Bytes(live.begin(), live.end()),
                    "Startup history was reordered behind newer live output");
            const auto replayDeadline = GetTickCount64() + 5000;
            main.HandleMessage(WM_APP + 1, 0, 0);
            while (!main.received_.empty() && GetTickCount64() < replayDeadline) paintOpportunity();
            if (!main.received_.empty() || main.activeSession_->terminal.PlainText(false).find(L"LIVE_AFTER_HISTORY") == std::wstring::npos) {
                const auto text = main.activeSession_->terminal.PlainText(false);
                std::cerr << "FAILED startup replay state: remaining=" << main.received_.size()
                          << " continuation=" << main.receiveContinuation_ << " posted=" << main.receivePosted_
                          << " update=" << GetUpdateRect(parent, nullptr, FALSE)
                          << " active=" << main.activeSession_->id << " textLength=" << text.size()
                          << " elapsed=" << GetTickCount64() - (replayDeadline - 5000)
                          << " tail=" << WideToMultiByte(text.substr(text.size() > 64 ? text.size() - 64 : 0), CP_UTF8)
                          << std::endl;
            }
            Require(main.received_.empty() && main.activeSession_->terminal.PlainText(false).find(L"LIVE_AFTER_HISTORY") != std::wstring::npos,
                    "Sliced startup replay lost newer live data");
            SetWindowPos(parent, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        main.powerService_.Shutdown();
        main.aiPane_.Shutdown();
        DestroyWindow(parent);
        for (auto font : {main.uiFont_, main.smallFont_, main.titleFont_, main.terminalFont_}) DeleteObject(font);
    }
    static void RunShutdown() {
        test::Wsa sockets;
        MainWindow main;
        Require(main.Create(GetModuleHandleW(nullptr), SW_SHOWNOACTIVATE), "Create native shutdown window");
        const HWND window = main.window_;
        auto startup = std::make_shared<HeldTransport>();
        auto announced = std::make_shared<HeldTransport>();
        auto pending = std::make_shared<HeldTransport>();
        auto active = std::make_shared<HeldTransport>();
        Require(startup->release && startup->entered && announced->release && announced->entered &&
                pending->release && pending->entered &&
                active->release && active->entered, "Create shutdown gates");
        struct ReleaseGates {
            std::shared_ptr<HeldTransport> startup, announced, pending, active;
            ~ReleaseGates() { SetEvent(startup->release); SetEvent(announced->release);
                              SetEvent(pending->release); SetEvent(active->release); }
        } release{startup, announced, pending, active};
        auto session = std::make_unique<MainWindow::SessionState>();
        session->id = 500; session->mode = 3; session->name = L"held startup";
        session->connection = pending; session->logger = std::make_shared<SessionLogger>();
        main.pendingSession_ = std::move(session);
        main.connectionThread_ = std::thread([startup] {
            SetEvent(startup->entered);
            WaitForSingleObject(startup->release, INFINITE);
            // Simulate success immediately before cancellation was observed.
        });
        Require(WaitForSingleObject(startup->entered, 3000) == WAIT_OBJECT_0, "Held startup did not begin");
        const auto disconnectStarted = GetTickCount64();
        main.DisconnectAll();
        const auto disconnectMs = GetTickCount64() - disconnectStarted;
        Require(disconnectMs < 250, "Disconnect all waited for the held connection thread");
        Require(pending->cancelled && main.pendingSession_ && main.cancelledConnectionPending_,
                "Pending connection was freed before its worker finished");
        SetEvent(startup->release);
        auto pump = [] {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) continue;
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        };
        const auto cancelledDeadline = GetTickCount64() + 5000;
        while (main.pendingSession_ && GetTickCount64() < cancelledDeadline) { pump(); Sleep(1); }
        Require(!main.pendingSession_ && !main.connectionThread_.joinable(),
                "Canceled connection was not reclaimed after its worker finished");
        Require(WaitForSingleObject(pending->entered, 3000) == WAIT_OBJECT_0 && pending->connected,
                "Racing successful startup was not stopped by owned background cleanup");
        pump();
        Require(IsWindow(window), "Pending connection cleanup blocked or destroyed the window");
        SetEvent(pending->release);

        session = std::make_unique<MainWindow::SessionState>();
        session->id = 502; session->mode = 3; session->name = L"held completion";
        session->connection = std::make_shared<Transport>();
        session->logger = std::make_shared<SessionLogger>();
        main.pendingSession_ = std::move(session);
        main.connectionThread_ = std::thread([announced] {
            SetEvent(announced->entered);
            WaitForSingleObject(announced->release, INFINITE);
        });
        Require(WaitForSingleObject(announced->entered, 3000) == WAIT_OBJECT_0, "Held completion did not begin");
        main.connectionResultReady_ = true;
        main.connectionResultSuccess_ = false;
        const auto completionStarted = GetTickCount64();
        main.FinishPendingConnection();
        const auto completionMs = GetTickCount64() - completionStarted;
        Require(completionMs < 250 && main.pendingSession_, "Connection notification joined an unfinished worker");
        SetEvent(announced->release);
        const auto completionDeadline = GetTickCount64() + 5000;
        while (main.pendingSession_ && GetTickCount64() < completionDeadline) { pump(); Sleep(1); }
        Require(!main.pendingSession_ && !main.connectionThread_.joinable(),
                "Connection completion did not resume after worker exit");

        session = std::make_unique<MainWindow::SessionState>();
        session->id = 501; session->mode = 1; session->name = L"held live transport";
        session->connection = active; session->logger = std::make_shared<SessionLogger>();
        main.sessions_.push_back(std::move(session));
        const auto closeStarted = GetTickCount64();
        SendMessageW(window, WM_CLOSE, 0, 0);
        const auto closeMs = GetTickCount64() - closeStarted;
        Require(closeMs < 250, "Window close waited for the held transport");
        Require(main.Closing() && IsWindow(window) && !IsWindowVisible(window),
                "Window was destroyed before the held transport was reclaimed");
        Require(WaitForSingleObject(active->entered, 3000) == WAIT_OBJECT_0, "Shutdown worker did not stop transport");
        PostMessageW(window, WM_NULL, 0, 0);
        pump();
        Require(IsWindow(window) && active->connected, "Shutdown destroyed the window while the worker was held");
        SendMessageW(window, WM_CLOSE, 0, 0);
        Require(IsWindow(window), "Repeated close destroyed the window during cleanup");
        SetEvent(active->release);
        const auto closeDeadline = GetTickCount64() + 5000;
        while (IsWindow(window) && GetTickCount64() < closeDeadline) { pump(); Sleep(1); }
        Require(!IsWindow(window) && !active->connected && main.shutdownComplete_,
                "Window or transport survived completed shutdown");
        const Json evidence{{"disconnectAllMs", disconnectMs}, {"connectionCompletionMs", completionMs},
                            {"closeHandlerMs", closeMs},
                            {"pendingStartupHeld", true}, {"transportStopHeld", true},
                            {"scope", "native window and message loop with controlled blocking transports; no hardware latency claim"}};
        std::cout << evidence.dump() << '\n';
        wchar_t directory[32768]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", directory, 32768))
            std::ofstream(std::filesystem::path(directory) / L"shutdown-responsiveness.json") << evidence.dump(2);
    }
};
}
int main() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput startup; ULONG_PTR token{}; Gdiplus::GdiplusStartup(&token, &startup, nullptr);
    HWND parent = CreateWindowExW(0,L"STATIC",L"AI device callback fixture",WS_OVERLAPPEDWINDOW,0,0,1000,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    int result = 0;
    try { serialctl::AiDeviceTestAccess::Run(parent); serialctl::AiDeviceTestAccess::RunShutdown(); std::cout << "[PASS] Actual desktop AI callback and asynchronous connection/shutdown lifecycle; controlled transports, no physical devices\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; result = 1; }
    DestroyWindow(parent); Gdiplus::GdiplusShutdown(token); return result;
}
