#include "MainWindow.h"
#include "Win32Helpers.h"
#include "resource.h"
#include <gdiplus.h>
#include <iostream>
#include <stdexcept>

namespace serialctl {
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
        main.CreateControls();
        ShowWindow(parent, SW_SHOW);
        GetClientRect(parent, &viewport);
        main.pendingMode_ = 3;
        const auto remote = CreateDialogParamW(main.instance_, MAKEINTRESOURCEW(IDD_CONNECTION), parent,
                                               MainWindow::ConnectionDialogProc, reinterpret_cast<LPARAM>(&main));
        Require(remote != nullptr, "Remote connection dialog missing");
        RECT dialogBounds{};
        GetClientRect(remote, &dialogBounds);
        for (int id : {IDC_HOST, IDC_SERIAL_REFRESH, IDC_REMOTE_MODE, IDC_SERIAL, IDC_DIALOG_ERROR, IDCANCEL, IDOK}) {
            RECT control{};
            GetWindowRect(GetDlgItem(remote, id), &control);
            MapWindowPoints(nullptr, remote, reinterpret_cast<POINT *>(&control), 2);
            Require(control.left >= 0 && control.top >= 0 && control.right <= dialogBounds.right &&
                        control.bottom <= dialogBounds.bottom,
                    "Remote discovery controls escape compact dialog");
        }
        Require(SendDlgItemMessageW(remote, IDC_REMOTE_MODE, CB_GETCOUNT, 0, 0) == 3,
                "Remote transport options missing");
        for (int id : {IDC_REMOTE_MODE, IDC_SERIAL}) {
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
                HWND button = GetDlgItem(main.powerPane_.Handle(), id);
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
            const std::wstring ending = mode == 4 ? L"\r\n" : mode == 0 ? L"\n" : L"\r";
            auto expected = WideToMultiByte((mode == 4 ? L"echo 中文" : L"echo evidence") + ending, transports[mode]->input);
            Require(transports[mode]->sent == Bytes(expected.begin(), expected.end()), "AI wrote wrong target, encoding or line ending");
        }
        main.SwitchSession(4, MainWindow::SessionFocus::Preserve);
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
        for (int i = 0; i < 5; ++i) {
            SendMessageW(main.commandList_, i == 1 ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, MK_LBUTTON, runPoint);
            SendMessageW(main.commandList_, WM_LBUTTONUP, 0, runPoint);
        }
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
        for (int i = 0; i < 3; ++i)
            SendMessageW(GetDlgItem(main.powerPane_.Handle(), 20 + i), BM_SETCHECK, i == 1 ? BST_UNCHECKED : BST_CHECKED, 0);
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
        SendMessageW(GetDlgItem(main.powerPane_.Handle(), 20), BM_SETCHECK, BST_UNCHECKED, 0);
        Require(main.AiDeviceRequest("power.output", args).contains("error"), "Changed channel selection was silently redirected");
        SendMessageW(GetDlgItem(main.powerPane_.Handle(), 20), BM_SETCHECK, BST_CHECKED, 0);
        args["enabled"] = false; args["requestId"] = "ai-device-fixture-off";
        Require(Wait(main.powerService_, main.AiDeviceRequest("power.output", args))["state"] == "completed", "Power OFF failed");
        Require(main.powerService_.State()["channels"][0]["output"] == false && main.powerService_.State()["channels"][2]["output"] == false, "Power OFF readback incorrect");
        Require(main.AiDeviceRequest("power.parameters", {{"voltage", 30}}).contains("error"), "AI gained unrequested parameter operation");
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
