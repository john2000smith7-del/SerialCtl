#include "AiPane.h"
#include "HostKeyDialog.h"
#include "UiTestEvidence.h"
#include <chrono>
#include <cmath>
#include <gdiplus.h>
#include <iostream>
#include <stdexcept>


namespace {
struct HostDialogProbe {
    bool accept = false, dark = false;
    int dpi = 96;
    HWND pending = nullptr;
    std::wstring image;
    std::string error;
    std::wstring expectedNew;
    static BOOL CALLBACK Find(HWND h, LPARAM data) {
        auto self = reinterpret_cast<HostDialogProbe*>(data);
        if (!IsWindowVisible(h) || !GetDlgItem(h, 4100) || !GetDlgItem(h, IDOK)) return TRUE;
        if (self->pending != h) { self->pending = h; SetForegroundWindow(h); UpdateWindow(h); return TRUE; }
        try {
            auto require = [](bool ok, const char* why) { if (!ok) throw std::runtime_error(why); };
            require(IsDlgButtonChecked(h, 4100) == BST_UNCHECKED && !IsWindowEnabled(GetDlgItem(h, IDOK)),
                    "Host identity approval is preselected or enabled without verification");
            RECT check{}, action{}, client{};
            GetWindowRect(GetDlgItem(h, 4100), &check); GetWindowRect(GetDlgItem(h, IDOK), &action);
            require(check.bottom < action.top, "Consent overlaps the fixed action row on a small work area");
            MapWindowPoints(HWND_DESKTOP, h, reinterpret_cast<POINT*>(&action), 2); GetClientRect(h, &client);
            require(action.left >= 0 && action.right <= client.right && action.bottom <= client.bottom,
                    "Host identity actions escaped the visible dialog");
            SendMessageW(h, WM_COMMAND, IDOK, 0);
            require(IsWindow(h), "Enter/direct IDOK bypassed the verification checkbox");
            wchar_t text[2048]{}; GetDlgItemTextW(h, 4102, text, 2048);
            require(std::wstring(text).find(self->expectedNew) != std::wstring::npos,
                    "New identity is truncated in the copyable native field");
            serialctl::ui_test::SaveUiEvidence(h, self->image + L"-unchecked.png");
            GetWindowRect(GetDlgItem(h, 4100), &check);
            MapWindowPoints(HWND_DESKTOP, h, reinterpret_cast<POINT*>(&check), 2);
            auto dc = GetDC(h);
            const auto gap = GetPixel(dc, MulDiv(20, self->dpi, 96), check.bottom + MulDiv(4, self->dpi, 96));
            ReleaseDC(h, dc);
            require(gap == serialctl::UiTheme(self->dark).surface,
                    "A scrolling fingerprint card painted across the fixed consent footer");
            if (self->accept) {
                SendDlgItemMessageW(h, 4100, BM_CLICK, 0, 0);
                require(IsDlgButtonChecked(h, 4100) == BST_CHECKED && IsWindowEnabled(GetDlgItem(h, IDOK)),
                        "Explicit verification did not enable the confirmation action");
                serialctl::ui_test::SaveUiEvidence(h, self->image + L"-checked.png");
            }
        } catch (const std::exception& failure) { self->error = failure.what(); }
        PostMessageW(h, WM_COMMAND, self->error.empty() && self->accept ? IDOK : IDCANCEL, 0);
        return FALSE;
    }
    static void CALLBACK Tick(HWND parent, UINT, UINT_PTR, DWORD) {
        auto self = reinterpret_cast<HostDialogProbe*>(GetPropW(parent, L"SerialCtl.TestHostDialog"));
        if (self) EnumThreadWindows(GetCurrentThreadId(), Find, reinterpret_cast<LPARAM>(self));
    }
};
void CheckHostKeyDialogs(HWND parent, HFONT font) {
    using namespace serialctl;
    const std::wstring oldKey = L"ssh-ed25519 255 SHA256:44uodpMmNt68MktdLXKNyvUzZhOewpe0uE2NpYtDoHM";
    const std::wstring newKey = L"ssh-ed25519 255 SHA256:zjS0PTSjix4sGz1bW4OLroa4lt3rkVQawAcHWb+8nd0";
    for (int dpi : {96, 192}) {
    LOGFONTW metrics{}; GetObjectW(font, sizeof(metrics), &metrics); metrics.lfHeight = MulDiv(metrics.lfHeight, dpi, 96);
    HFONT scaled = CreateFontIndirectW(&metrics);
    for (bool dark : {false, true}) {
        for (auto reason : {HostKeyReason::Changed, HostKeyReason::FirstConnection, HostKeyReason::Reverify}) {
            HostKeyChallenge challenge{dpi == 96 ? L"192.0.2.20" : std::wstring(120, L'a') + L".example.invalid", 22, oldKey, newKey, reason};
            if (reason == HostKeyReason::FirstConnection) challenge.previousFingerprint.clear();
            HostDialogProbe probe;
            probe.dark = dark;
            probe.dpi = dpi;
            probe.accept = reason != HostKeyReason::Reverify;
            probe.expectedNew = newKey.substr(newKey.find(L"SHA256:"));
            probe.image = std::wstring(L"v206-host-key-") + (dark ? L"dark-" : L"light-") + (dpi == 96 ? L"" : L"192dpi-") +
                (reason == HostKeyReason::Changed ? L"changed" : reason == HostKeyReason::FirstConnection ? L"first" : L"reverify");
            SetPropW(parent, L"SerialCtl.TestHostDialog", &probe);
            SetTimer(parent, 9206, 50, HostDialogProbe::Tick);
            const bool accepted = HostKeyDialog::Show(parent, challenge, scaled ? scaled : font, dark, dpi);
            KillTimer(parent, 9206); RemovePropW(parent, L"SerialCtl.TestHostDialog");
            if (!probe.error.empty()) throw std::runtime_error(probe.error);
            if (accepted != probe.accept) throw std::runtime_error("Host identity dialog returned the wrong consent decision");
        }
    }
    if (scaled) DeleteObject(scaled);
    }
}
}

namespace serialctl {
struct AiPaneTestAccess {
    static void Run(AiPane &pane) {
        auto require = [](bool value, const char *error) {
            if (!value)
                throw std::runtime_error(error);
        };
        // A completed connection introduces capabilities locally, without
        // authorizing a model turn or duplicating the greeting on reconnect.
        auto configure = [&] {
            pane.requests_[1001] = {"config", {}};
            pane.Event({{"event", "response"}, {"token", 1001},
                        {"result", {{"defaultModel", "fixture"}}}});
        };
        configure();
        require(pane.messages_.size() == 1 && pane.messages_.front().role == L"AI",
                "Successful connection must introduce the engineering assistant once");
        const auto introduction = pane.messages_.front().text;
        require(introduction.find(L"高级嵌入式软件工程师助手") != std::wstring::npos &&
                    introduction.find(L"Jojo") == std::wstring::npos &&
                    introduction.find(L"jiojio") == std::wstring::npos &&
                    introduction.find(L"电源") == std::wstring::npos,
                "Public introduction exposes the internal name or advertises unavailable power control");
        configure();
        require(pane.messages_.size() == 1 && !pane.busy_ && !pane.taskAuthorized_ && pane.requests_.empty(),
                "Reconnect duplicated the introduction or started an unsolicited task");
        pane.NewConversation();
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
            for (int width : {228, 260, 368, 400, 900}) {
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

        pane.busy_ = true;
        pane.stopRequested_ = false;
        pane.Layout();
        RECT sendArea{}, stopArea{};
        GetWindowRect(pane.Control(11), &sendArea);
        GetWindowRect(pane.Control(21), &stopArea);
        RECT overlap{};
        require(IsWindowVisible(pane.Control(21)) && !IntersectRect(&overlap, &sendArea, &stopArea),
                "Busy Stop must have a separate hit rectangle from Send");
        for (int i = 0; i < 5; ++i) pane.Action(11);
        require(!pane.stopRequested_, "Repeated Send clicks stopped the active task");
        pane.Action(21);
        require(pane.stopRequested_, "Explicit Stop did not request cancellation");
        pane.busy_ = pane.stopRequested_ = false;
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
        {
            RECT size{};
            GetClientRect(pane.transcript_, &size);
            auto screen = GetDC(pane.transcript_), dc = CreateCompatibleDC(screen);
            auto bitmap = CreateCompatibleBitmap(screen, size.right, size.bottom);
            auto old = SelectObject(dc, bitmap);
            pane.PaintTranscript(dc);
            const auto summaryPixel = GetPixel(dc, pane.messages_[0].bounds.left + pane.D(20),
                                               pane.messages_[0].bounds.top + pane.D(34) - pane.scroll_.offset);
            SelectObject(dc, old);
            DeleteObject(bitmap);
            DeleteDC(dc);
            ReleaseDC(pane.transcript_, screen);
            require(summaryPixel == UiTheme(pane.dark_).surface,
                    "Collapsed process summary must remain visibly painted and expandable");
        }

        pane.collapsedGroups_.erase("process-fixture");
        pane.Reflow();
        require(!pane.messages_[0].folded, "Process could not be expanded");
        pane.displayGroup_ = "failed-fixture";
        pane.busy_ = true;
        pane.groupStates_[pane.displayGroup_] = L"含失败记录";
        pane.Add(L"工具", L"退出码 1", "failed-tool");
        const auto failedTool = pane.messages_.size() - 1;
        pane.Add(L"AI", L"已根据日志完成分析", "recovered-final");
        require(!pane.messages_[failedTool].folded, "Running process must stay expanded");
        pane.Event({{"event", "taskFinished"}, {"status", "completed"}});
        require(pane.messages_[failedTool].folded && !pane.messages_.back().folded &&
                    pane.groupStates_["failed-fixture"] == L"已完成 · 含失败记录",
                "A completed answer must collapse diagnostic failures while retaining their summary");
        require(pane.messages_[failedTool].text == L"退出码 1", "Folding lost a failed command's evidence");
        for (const bool dark : {false, true}) {
            pane.NewConversation();
            pane.Theme(dark);
            pane.thread_ = "phase-less-thread";
            pane.displayGroup_ = "phase-less-turn";
            pane.busy_ = true;
            pane.Event({{"event", "textComplete"}, {"messageId", "progress"}, {"text", "Checking logs"}, {"final", true}});
            pane.Add(L"工具", L"完整工具输出", "phase-less-tool");
            pane.Event({{"event", "textComplete"}, {"messageId", "answer"}, {"text", "Final diagnosis"}, {"final", true}});
            require(!pane.messages_[0].process && !pane.messages_[1].folded,
                    "Process collapsed before the turn actually finished");
            const std::wstring palette = dark ? L"dark" : L"light";
            ui_test::SaveUiEvidence(pane.transcript_, L"v205-process-" + palette + L"-running.png",
                                 [&](HDC dc) { pane.PaintTranscript(dc); });
            pane.Event({{"event", "taskFinished"}, {"status", "completed"}});
            require(pane.messages_[0].process && pane.messages_[0].folded && pane.messages_[1].hidden &&
                        !pane.messages_[2].process && !pane.messages_[2].hidden,
                    "Phase-less progress did not auto-collapse separately from the final answer");
            ui_test::SaveUiEvidence(pane.transcript_, L"v205-process-" + palette + L"-completed.png",
                                 [&](HDC dc) { pane.PaintTranscript(dc); });
            const auto &summary = pane.messages_.front().bounds;
            const auto toggle = MAKELPARAM(summary.right - pane.D(20), summary.top + pane.D(20) - pane.scroll_.offset);
            SendMessageW(pane.transcript_, WM_LBUTTONDOWN, MK_LBUTTON, toggle);
            SendMessageW(pane.transcript_, WM_LBUTTONUP, 0, toggle);
            require(!pane.messages_[0].folded && !pane.messages_[1].hidden &&
                        pane.messages_[1].text == L"完整工具输出",
                    "Auto-collapsed process cannot be reopened using its real transcript action");
            ui_test::SaveUiEvidence(pane.transcript_, L"v205-process-" + palette + L"-expanded.png",
                                 [&](HDC dc) { pane.PaintTranscript(dc); });
        }
        for (const std::string outcome : {"failed", "interrupted"}) {
            pane.NewConversation();
            pane.displayGroup_ = "unfinished-turn";
            pane.busy_ = true;
            pane.stopRequested_ = outcome == "interrupted";
            pane.Add(L"工具", L"未完成输出", "unfinished-tool");
            pane.Event({{"event", "taskFinished"}, {"status", outcome}});
            require(!pane.messages_.front().folded, "Failed or stopped turn hid its unresolved process");
        }
        pane.NewConversation();
        pane.stopRequested_ = false;
        pane.requests_[1002] = {"resume", {}};
        pane.busy_ = true;
        pane.Event({{"event", "response"}, {"token", 1002}, {"result", {
            {"conversationId", "restored-thread"}, {"messages", {
                {{"role", "AI"}, {"text", "Checking"}, {"messageId", "restored-progress"},
                 {"group", "restored-turn"}, {"status", "completed"}, {"failedStep", true}},
                {{"role", "工具"}, {"text", "exit 1"}, {"messageId", "restored-tool"},
                 {"group", "restored-turn"}, {"status", "completed"}, {"failedStep", true}},
                {{"role", "AI"}, {"text", "Diagnosis"}, {"messageId", "restored-answer"},
                 {"group", "restored-turn"}, {"status", "completed"}, {"failedStep", true}}}}}}});
        require(pane.messages_[0].folded && pane.messages_[1].hidden && !pane.messages_[2].process &&
                    pane.groupStates_["restored-turn"] == L"已完成 · 含失败记录",
                "Restoring a completed turn reopened its process or lost failed-step information");
        pane.NewConversation();
        pane.thread_ = "fixture";
        pane.NewConversation();
        pane.Add(L"我", L"中文 ABC\n```\nint main(void) {}\n```");
        for (bool dark : {false, true}) {
            pane.Theme(dark);
            RECT size{};
            GetClientRect(pane.transcript_, &size);
            auto screen = GetDC(pane.transcript_);
            auto dc = CreateCompatibleDC(screen);
            auto bitmap = CreateCompatibleBitmap(screen, size.right, size.bottom);
            auto old = SelectObject(dc, bitmap);
            pane.PaintTranscript(dc);
            const auto &message = pane.messages_.front();
            for (const auto &paragraph : message.paragraphs) {
                if (paragraph.style != 0 && paragraph.style != 2)
                    continue;
                unsigned ink = 0;
                for (int y = paragraph.rect.top; y < paragraph.rect.bottom - 4; y += 2)
                    for (int x = paragraph.rect.left; x < paragraph.rect.right; x += 2) {
                        const auto pixel =
                            GetPixel(dc, message.bounds.left + x, message.bounds.top + y - pane.scroll_.offset);
                        if (GetRValue(pixel) > 230 && GetGValue(pixel) > 230 && GetBValue(pixel) > 230)
                            ++ink;
                    }
                require(ink > 3,
                        "User prose and code must visibly render light ink on their blue surfaces in both themes");
                if (paragraph.style == 2) {
                    const auto background = GetPixel(dc, message.bounds.left + paragraph.rect.right - 8,
                                                     message.bounds.top + paragraph.rect.top + 8 - pane.scroll_.offset);
                    auto linear = [](unsigned v) {
                        const double c = v / 255.0;
                        return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
                    };
                    const double luminance = .2126 * linear(GetRValue(background)) +
                                             .7152 * linear(GetGValue(background)) +
                                             .0722 * linear(GetBValue(background));
                    require(1.05 / (luminance + .05) >= 4.5,
                            "User code block background loses readable contrast with white ink");
                }
            }
            SelectObject(dc, old);
            DeleteObject(bitmap);
            DeleteDC(dc);
            ReleaseDC(pane.transcript_, screen);
        }
        pane.messageBytes_ = 4 * 1024 * 1024;
        const auto displayed = pane.messages_.size();
        pane.Add(L"AI", L"overflow");
        pane.Add(L"AI", L"overflow");
        require(pane.messages_.size() == displayed + 1 && pane.messages_.back().item == "client:display-limit",
                "Long transcript must show one visible bounded notice instead of silently dropping output");
        pane.NewConversation();
        pane.thread_ = "fixture";
        ai::ServerSettings server;
        server.host = L"serialctl-pane-" + std::to_wstring(GetCurrentProcessId()) + L".invalid";
        server.username = L"fixture";
        server.password = L"fixture-only-π";
        require(pane.client_.Connect(
                    server, [](ai::Json) {},
                    [](const HostKeyChallenge&) { return true; }),
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
    SetProcessDPIAware();
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
            CheckHostKeyDialogs(parent, font);
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
