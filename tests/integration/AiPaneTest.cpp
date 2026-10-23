#include "AiPane.h"
#include <chrono>
#include <gdiplus.h>
#include <iostream>
#include <stdexcept>

namespace serialctl {
struct AiPaneTestAccess {
    static void Run(AiPane &pane) {
        auto require = [](bool value, const char *error) {
            if (!value)
                throw std::runtime_error(error);
        };
        wchar_t modelClass[32]{};
        GetClassNameW(pane.Control(12), modelClass, 32);
        require(_wcsicmp(modelClass, L"Static") == 0 && !pane.Control(13),
                "Model must be read-only server default without refresh/manual controls");
        require(pane.target_.empty(), "AI must not select a local serial port by default");
        require(SendMessageW(pane.Control(14), CB_GETCOUNT, 0, 0) == 3,
                "Target picker must offer explicit selection plus remote-IP discovery");
        require(SendMessageW(pane.Control(14), CB_GETCURSEL, 0, 0) == 0,
                "Target picker default must be its placeholder");
        SendMessageW(pane.Control(14), CB_SETCURSEL, 1, 0);
        SendMessageW(pane.Handle(), WM_COMMAND, MAKEWPARAM(14, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(pane.Control(14)));
        require(pane.target_ == "session-7", "Explicit remote serial selection was not associated with the task");
        pane.Add(L"AI", L"ABC中文DEF\n```\nx = 1;\n```\nend");
        const auto &first = pane.messages_.front();
        const auto &line = first.paragraphs.front().lines.front();
        const int y = first.bounds.top + line.rect.top + 2;
        const int x = first.bounds.left + line.rect.left;
        SendMessageW(pane.transcript_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x + line.advances[1], y));
        SendMessageW(pane.transcript_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + line.advances[5], y));
        SendMessageW(pane.transcript_, WM_LBUTTONUP, 0, MAKELPARAM(x + line.advances[5], y));
        require(pane.SelectedText() == L"BC中文",
                "Character range selection differs from actual displayed glyph positions");
        require(OpenClipboard(pane.Handle()) != FALSE, "Cannot inspect selection clipboard");
        auto data = GetClipboardData(CF_UNICODETEXT);
        auto copied = static_cast<const wchar_t *>(GlobalLock(data));
        const std::wstring clip = copied ? copied : L"";
        if (copied)
            GlobalUnlock(data);
        CloseClipboard();
        require(clip == L"BC中文", "Releasing text selection must copy selected characters, not whole message");
        pane.selectionStart_ = {0, 1, 6};
        pane.selectionEnd_ = {0, 0, 0};
        require(pane.SelectedText() == L"ABC中文DEF\r\nx = 1;",
                "Reverse selection must span code and preserve hard line breaks");
        pane.ClearTextSelection();
        pane.messages_.clear();
        pane.messageBytes_ = 0;
        pane.scroll_ = {};
        pane.thread_ = "fixture";
        for (int i = 0; i < 40; ++i)
            pane.Add(
                i % 2 ? L"AI" : L"我",
                L"历史消息 " + std::to_wstring(i) +
                    L"\n真实串口日志的事实和待验证判断必须区分。\n```\nWaiting for root device /dev/mmcblk0p3...\n```");
        require(pane.scroll_.Maximum() > 0, "Long chat did not create a scrollable transcript");
        require(pane.scroll_.offset == pane.scroll_.Maximum(), "Live chat should initially follow latest output");
        SendMessageW(pane.transcript_, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(WHEEL_DELTA)), 0);
        const int reading = pane.scroll_.offset;
        require(reading < pane.scroll_.Maximum(), "Mouse wheel did not scroll chat history");
        pane.Event({{"event", "textDelta"},
                    {"conversationId", "fixture"},
                    {"messageId", "new-reply"},
                    {"text", u8"新的流式消息"}});
        require(pane.scroll_.offset == reading && pane.scroll_.unread,
                "Streaming reply stole the history reading position");
        require(IsWindowVisible(pane.Control(20)), "Unread output must expose return-to-latest action");
        SendMessageW(pane.transcript_, WM_KEYDOWN, VK_HOME, 0);
        require(pane.scroll_.offset == 0, "Home must reach oldest displayed message");
        SendMessageW(pane.transcript_, WM_KEYDOWN, VK_END, 0);
        require(pane.scroll_.offset == pane.scroll_.Maximum() && !pane.scroll_.unread,
                "End must return to latest output");
        SendMessageW(pane.transcript_, WM_KEYDOWN, VK_PRIOR, 0);
        require(pane.scroll_.offset < pane.scroll_.Maximum(), "PageUp must scroll the message viewport");
        auto thumb = pane.ScrollThumb();
        SendMessageW(pane.transcript_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(thumb.left + 1, thumb.top + 1));
        SendMessageW(pane.transcript_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(thumb.left + 1, 0));
        SendMessageW(pane.transcript_, WM_LBUTTONUP, 0, MAKELPARAM(thumb.left + 1, 0));
        require(pane.scroll_.offset == 0, "Dragging the scrollbar to the top failed");
        for (bool dark : {false, true}) {
            pane.Theme(dark);
            for (int width : {368, 400, 900}) {
                SetWindowPos(pane.Handle(), nullptr, 0, 0, width, 600, SWP_NOZORDER);
                pane.Layout();
                RECT input{}, outer{}, canvas{}, target{}, send{};
                GetWindowRect(pane.Control(10), &input);
                GetWindowRect(pane.Handle(), &outer);
                GetWindowRect(pane.transcript_, &canvas);
                GetWindowRect(pane.Control(14), &target);
                GetWindowRect(pane.Control(11), &send);
                require(input.left >= outer.left && input.right <= outer.right && input.bottom <= outer.bottom,
                        "Composer escapes center pane");
                require(canvas.bottom <= input.top, "Transcript must not cover the fixed composer");
                require(target.right <= send.left, "Target selector overlaps Send at narrow width");
                require(pane.scroll_.offset == 0, "Theme/resize must preserve the history reading position");
                UpdateWindow(pane.Handle());
                UpdateWindow(pane.transcript_);
            }
        }
        pane.Action(20);
        require(pane.scroll_.offset == pane.scroll_.Maximum(), "Return-to-latest button failed");
        for (const auto &message : pane.messages_)
            for (const auto &paragraph : message.paragraphs)
                require(paragraph.style != 3, "Message role/status labels must not appear inside bubbles");
        // The edit's full-window print includes its nonclient rounded corners.
        // Dark theme previously filled those corners with terminal black.
        RECT inputBounds{};
        GetWindowRect(pane.Control(10), &inputBounds);
        auto screen = GetDC(pane.Control(10));
        auto buffer = CreateCompatibleDC(screen);
        auto bitmap =
            CreateCompatibleBitmap(screen, inputBounds.right - inputBounds.left, inputBounds.bottom - inputBounds.top);
        auto old = SelectObject(buffer, bitmap);
        SendMessageW(pane.Control(10), WM_PRINT, reinterpret_cast<WPARAM>(buffer), PRF_CLIENT | PRF_NONCLIENT);
        const auto corner = GetPixel(buffer, 0, 0);
        EnableWindow(pane.Control(10), FALSE);
        SendMessageW(pane.Control(10), WM_PRINT, reinterpret_cast<WPARAM>(buffer), PRF_CLIENT | PRF_NONCLIENT);
        const auto disabledField = GetPixel(buffer, (inputBounds.right - inputBounds.left) / 2,
            (inputBounds.bottom - inputBounds.top) / 2);
        EnableWindow(pane.Control(10), TRUE);
        SelectObject(buffer, old);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        ReleaseDC(pane.Control(10), screen);
        require(corner == UiAiTheme(true).field, "Borderless edit must match its enclosing composer card");
        require(disabledField == UiAiTheme(true).field, "Disabled AI edit exposed a different rectangular fill");

        pane.NewConversation();
        pane.displayGroup_ = "process-fixture";
        pane.thread_ = "process-thread";
        pane.busy_ = true;
        pane.waitingReply_ = true;
        pane.Reflow();
        require(pane.waitingBounds_.bottom > pane.waitingBounds_.top, "Waiting indicator missing inside transcript");
        pane.Add(L"工具", L"实际执行输出", "process-tool");
        pane.Add(L"AI", L"最终回答", "process-final");
        pane.Event({{"event", "taskFinished"}, {"conversationId", "process-thread"}, {"status", "completed"}});
        require(!pane.waitingReply_ && pane.messages_[0].folded && !pane.messages_[1].folded,
                "Successful process must fold without hiding final answer");
        require(pane.messages_[0].text == L"实际执行输出", "Folding discarded original tool evidence");
        pane.collapsedGroups_.erase("process-fixture");
        pane.Reflow();
        require(!pane.messages_[0].folded, "Process could not be expanded");
        pane.displayGroup_ = "failed-fixture";
        pane.busy_ = true;
        pane.groupStates_[pane.displayGroup_] = L"含失败记录";
        pane.Add(L"工具", L"退出码 1", "failed-tool");
        pane.Event({{"event", "taskFinished"}, {"status", "completed"}});
        require(!pane.messages_.back().folded, "Failed operation must remain visible even when AI turn completes");
        pane.NewConversation();
        pane.thread_ = "fixture";
        ai::ServerSettings server;
        server.host = L"serialctl-pane-" + std::to_wstring(GetCurrentProcessId()) + L".invalid";
        server.username = L"fixture";
        server.password = L"fixture-only-π";
        require(pane.client_.Connect(
                    server, [](ai::Json) {},
                    [](const std::wstring &, std::uint16_t, const std::wstring &) { return true; }),
                "Cannot connect offline process fixture");
        const auto deadline = GetTickCount64() + 10000;
        while (!pane.client_.Ready() && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(pane.client_.Ready(), "Offline process fixture did not become ready");
        unsigned writes = 0;
        unsigned powerWrites = 0, powerActions = 0;
        pane.devices_ = [&](const std::string &operation, const ai::Json &args) {
            if (operation == "command") {
                require(args.at("sessionId") == "session-7", "Command escaped selected serial target");
                ++writes;
                return ai::Json{{"accepted", 12}, {"execution", "unknown"}};
            }
            if (operation == "power.get")
                return ai::Json{{"connected", true}, {"identity", "offline power fixture"},
                    {"mode", "independent"}, {"selectedChannels", {1, 3}}};
            if (operation == "power.output") {
                require(args.at("channels") == ai::Json::array({1,3}), "Power escaped GUI-selected channels");
                require(args.at("requestId").get<std::string>().rfind("ai:",0) == 0, "Power lacks unique request identity");
                ++powerWrites;
                return ai::Json{{"id", "power-action-fixture"}, {"state", "queued"}};
            }
            if (operation == "power.action") {
                ++powerActions;
                return ai::Json{{"id", args.at("id")}, {"state", "failed"}, {"channels", {{{"state", "unknown"}}}}};
            }
            return ai::Json::array();
        };
        pane.busy_ = true;
        pane.taskAuthorized_ = true;
        pane.turn_ = "turn-direct";
        pane.turnTarget_ = "session-7";
        ai::Json command = {
            {"event", "serialToolRequest"},
            {"requestId", 900},
            {"conversationId", "fixture"},
            {"taskId", "turn-direct"},
            {"callId", "once"},
            {"tool", "serialctl_command"},
            {"arguments", {{"sessionId", "session-7"}, {"command", "dmesg | tail"}, {"reason", "diagnosis"}}}};
        pane.taskAuthorized_ = false;
        pane.Event(command);
        require(writes == 0, "History restore accepted a new device command");
        pane.taskAuthorized_ = true;
        pane.Event(command);
        require(writes == 1, "Selected serial command must execute directly without second confirmation");
        pane.Event(command);
        require(writes == 1, "Duplicate tool call resubmitted a serial command");
        command["callId"] = "other-target";
        command["arguments"]["sessionId"] = "session-other";
        pane.Event(command);
        require(writes == 1, "AI wrote another serial target");
        command["callId"] = "stopped";
        command["arguments"]["sessionId"] = "session-7";
        pane.stopRequested_ = true;
        pane.Event(command);
        require(writes == 1, "Stopped task still wrote the serial port");
        pane.stopRequested_ = false;

        // Power calls work without selecting a terminal, but require a current
        // read of the GUI-selected channels and the same active-turn checks.
        pane.turnTarget_.clear();
        ai::Json power = command;
        power["tool"] = "serialctl_power_output";
        power["callId"] = "power-without-state";
        power["arguments"] = {{"enabled", true}, {"reason", "User requested ON"}};
        pane.Event(power);
        require(powerWrites == 0, "Power output bypassed its current channel query");
        auto query = power;
        query["tool"] = "serialctl_power_get"; query["callId"] = "power-state"; query["arguments"] = ai::Json::object();
        pane.Event(query);
        power["callId"] = "power-on";
        pane.Event(power); pane.Event(power);
        require(powerWrites == 1, "Power was not direct or duplicate invocation was resubmitted");
        power["callId"] = "power-needs-fresh-state";
        pane.Event(power);
        require(powerWrites == 1, "A second power write used stale channel selection");
        query["callId"] = "power-state-again"; pane.Event(query);
        power["callId"] = "power-override"; power["arguments"]["channels"] = {2}; pane.Event(power);
        require(powerWrites == 1, "Model overrode GUI-selected power channels");
        auto action = power;
        action["tool"] = "serialctl_power_action"; action["callId"] = "power-result";
        action["arguments"] = {{"id", "power-action-fixture"}}; pane.Event(action);
        require(powerActions == 1, "Power readback action not routed");
        power["arguments"].erase("channels"); power["callId"] = "power-stopped";
        pane.stopRequested_ = true; pane.Event(power); pane.stopRequested_ = false;
        require(powerWrites == 1, "Stopped task still changed power output");

        pane.Event({{"event", "serverTool"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:running"}, {"completed", false}, {"text", "make test"}});
        pane.Event({{"event", "serverToolDelta"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:running"}, {"text", "ACTUAL_PROGRESS"}});
        pane.completionDeadline_ = GetTickCount64() - 1;
        pane.Poll();
        require(pane.busy_ && pane.nativeItems_.count("server:running"),
                "Compatibility completion interrupted an active native command");
        pane.Event({{"event", "serverTool"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:running"}, {"completed", true}, {"appendStream", true}, {"text", "exit 2: ACTUAL_FAILURE"}});
        require(pane.nativeItems_.empty(), "Completed native command stayed pending");
        size_t commandCards = 0;
        for (const auto& message : pane.messages_) if (message.item == "server:running") {
            ++commandCards;
            require(message.text == L"exit 2: ACTUAL_FAILURE\nACTUAL_PROGRESS", "Native result did not replace its progress");
        }
        require(commandCards == 1, "Native tool stream duplicated its message frame");
        pane.Event({{"event", "textComplete"},
                    {"conversationId", "fixture"},
                    {"taskId", "turn-direct"},
                    {"messageId", "final"},
                    {"text", "Done"},
                    {"final", true}});
        require(pane.completionDeadline_ != 0 && pane.busy_, "Final item did not arm compatible completion");
        pane.Event({{"event", "taskActivity"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"}});
        require(pane.completionDeadline_ == 0 && pane.busy_, "Subsequent activity did not cancel early completion");
        pane.Event({{"event", "textComplete"},
                    {"conversationId", "fixture"},
                    {"taskId", "turn-direct"},
                    {"messageId", "final"},
                    {"text", "Done"},
                    {"final", true}});
        pane.completionDeadline_ = GetTickCount64() - 1;
        pane.Poll();
        require(!pane.busy_, "Gateway without turn/completed left the conversation stuck");
        pane.busy_ = true;
        pane.turn_ = "next-turn";
        pane.Event({{"event", "taskFinished"},
                    {"conversationId", "fixture"},
                    {"taskId", "turn-direct"},
                    {"status", "completed"}});
        require(pane.busy_ && pane.turn_ == "next-turn", "Late completion ended the following task");
        pane.requests_[999] = {"interrupt", "turn-direct"};
        pane.Event({{"event", "response"}, {"token", 999}, {"result", ai::Json::object()}});
        require(pane.busy_ && pane.turn_ == "next-turn", "Late interrupt acknowledgement ended the following task");
        pane.Event({{"event", "serviceError"},
                    {"conversationId", "fixture"},
                    {"taskId", "next-turn"},
                    {"message", "Missing environment variable: `CONTAINER_APIKEY`."},
                    {"willRetry", false}});
        require(!pane.busy_ && pane.status_.find(L"CONTAINER_APIKEY") != std::wstring::npos,
                "Missing server environment did not release busy state with its actual cause");
        pane.client_.Shutdown();
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SerialCtl\\TrustedHostKeys", 0, KEY_SET_VALUE, &key) ==
            ERROR_SUCCESS) {
            RegDeleteValueW(key, (server.host + L":22").c_str());
            RegCloseKey(key);
        }
    }
};
} // namespace serialctl
int main() {
    using namespace serialctl;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &startup, nullptr);
    auto parent = CreateWindowExW(0, L"STATIC", L"AI pane offline UI test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                                  1100, 760, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    int result = 0;
    {
        AiPane pane;
        try {
            if (!pane.Create(parent, font, false, [](const std::string &method, const ai::Json &) {
                    if (method == "sessions")
                        return ai::Json::array({{{"id", "session-7"}, {"name", "192.0.2.20 / COM3"}}});
                    return ai::Json{{"error", "Offline UI test does not open or write devices"}};
                }))
                throw std::runtime_error("Cannot create native AI pane");
            SetWindowPos(pane.Handle(), nullptr, 0, 0, 1000, 650, SWP_NOZORDER | SWP_SHOWWINDOW);
            AiPaneTestAccess::Run(pane);
            std::cout << "AiPane: explicit remote selection, wheel/key/scrollbar history navigation, streaming scroll "
                         "anchoring, fixed composer geometry and both themes passed (Win32 UI smoke test)\n";
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            result = 1;
        }
        DestroyWindow(parent);
    }
    DeleteObject(font);
    Gdiplus::GdiplusShutdown(token);
    return result;
}
