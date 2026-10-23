#include "MainWindow.h"
#include "Win32Helpers.h"
#include <gdiplus.h>
#include <iostream>
#include <stdexcept>

namespace serialctl {
namespace {
void Require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Transport : IConnection {
    bool connected = true;
    unsigned input = CP_UTF8;
    Bytes sent;
    bool Start(DataCallback, StatusCallback, std::wstring&) override { return true; }
    void Stop() override { connected = false; }
    bool IsConnected() const override { return connected; }
    bool Send(const Bytes& data, std::wstring&) override { sent = data; return connected; }
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
        main.window_ = parent;
        main.instance_ = GetModuleHandleW(nullptr);
        RECT size{0, 0, 1400, 850};
        AdjustWindowRectEx(&size, WS_OVERLAPPEDWINDOW, FALSE, 0);
        SetWindowPos(parent, nullptr, 0, 0, size.right - size.left, size.bottom - size.top, SWP_NOZORDER);
        main.CreateControls();
        ShowWindow(parent, SW_SHOW);
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
        // Reuse the real terminal HWND and session model, not a copied AI log.
        main.ShowAiPage(false);
        main.aiPane_.SelectTarget("session-2");
        main.SyncAiTerminal();
        Require(main.aiVisible_ && main.activeSession_ == main.sessions_[1].get(),
            "AI target binding left the conversation page or selected wrong terminal");
        main.LayoutControls(1400, 850);
        Require(!main.aiMonitorCollapsed_, "AI monitor must start expanded");
        main.aiMonitorCollapsed_ = true;
        main.LayoutControls(1400, 850);
        Require(!IsWindowVisible(main.terminal_), "Collapsed AI monitor exposed terminal");
        main.aiMonitorCollapsed_ = false;
        main.aiMonitorRatio_ = 500;
        main.LayoutControls(1400, 850);
        RECT ai{}, terminal{}, header{};
        GetWindowRect(main.aiPane_.Handle(), &ai);
        GetWindowRect(main.terminal_, &terminal);
        GetWindowRect(main.aiTerminalHeader_, &header);
        Require(std::abs((terminal.right - terminal.left) - (ai.right - ai.left)) <= 1,
                "Default AI and terminal widths must be equal");
        Require(!main.sessions_[1]->terminal.LineWrapView(), "AI monitor defaults to long-line projection");
        Require(IsWindowVisible(main.terminal_) && terminal.left > ai.right && terminal.top >= header.bottom,
            "Expanded monitor did not place actual terminal beside chat under its header");
        Require(main.rightPanelWidth_ == 320 && !main.rightPanelCollapsed_,
            "AI monitor changed terminal sidebar preference");
        main.aiMonitorRatio_ = 600;
        main.LayoutControls(1400, 850);
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
        main.LayoutControls(1400, 850);
        GetWindowRect(main.terminal_, &terminal); GetWindowRect(main.aiPane_.Handle(), &ai);
        Require(!main.aiVisible_ && IsWindowVisible(main.terminal_) && terminal.left == ai.left &&
            main.rightPanelWidth_ == 320, "Leaving AI failed to restore original terminal layout");
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
