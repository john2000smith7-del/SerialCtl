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
    static void CALLBACK TechnicalTick(HWND parent, UINT, UINT_PTR timer, DWORD) {
        struct Find {
            static BOOL CALLBACK Each(HWND h, LPARAM) {
                wchar_t title[128]{}; GetWindowTextW(h, title, 128);
                if (std::wstring(title) != L"工具技术详情") return TRUE;
                ui_test::SaveUiEvidence(h, L"v209-tool-technical-details.png");
                PostMessageW(h, WM_COMMAND, IDCANCEL, 0);
                return FALSE;
            }
        };
        EnumThreadWindows(GetCurrentThreadId(), Find::Each, 0);
        KillTimer(parent, timer);
    }
    static void CheckV209(AiPane &pane) {
        auto check = [](bool value, const char *why) { if (!value) throw std::runtime_error(why); };
        pane.busy_ = false; pane.NewConversation();
        pane.busy_ = true; pane.waitingReply_ = true; pane.displayGroup_ = "v209-tools";
        const ai::Json result{{"sessionId", "session-1"}, {"text", u8"当前屏幕第一行\n当前屏幕第二行"},
            {"cursor", 8490}, {"alternateScreen", true}, {"truncated", false}};
        pane.AddTool("serialctl_screen", {{"sessionId", "session-1"}}, result, L"SSH");
        check(pane.messages_.front().folded && IsRectEmpty(&pane.waitingBounds_),
              "Tool process or waiting placeholder escaped its group");
        check(pane.messages_.front().text.find(L"sessionId") == std::wstring::npos &&
              pane.messages_.front().text.find(L"第一行\n当前屏幕第二行") != std::wstring::npos &&
              pane.messages_.front().technical.find(L"sessionId") != std::wstring::npos,
              "Readable tool text and optional technical envelope were not separated");
        for (bool dark : {false, true}) {
            pane.Theme(dark);
            pane.collapsedGroups_.erase("v209-tools"); pane.Reflow();
            ui_test::SaveUiEvidence(pane.transcript_, dark ? L"v209-readable-tools-dark.png" : L"v209-readable-tools-light.png",
                [&](HDC dc) { pane.PaintTranscript(dc); });
            const auto parent = GetAncestor(pane.window_, GA_ROOT);
            SetTimer(parent, 9209, 40, TechnicalTick);
            pane.ShowTechnicalDetails(0);
            KillTimer(parent, 9209);
        }
        pane.busy_ = false;
        pane.scroll_.unread = true; pane.Layout();
        RECT latest{}, observe{}, overlap{};
        GetWindowRect(pane.Control(20), &latest); GetWindowRect(pane.Control(32), &observe);
        check(IsWindowVisible(pane.Control(32)) && !IsWindowVisible(pane.Control(16)) &&
              !IntersectRect(&overlap,&latest,&observe) && latest.top==observe.top && latest.left>=observe.right,
              "Latest and log-menu actions must have separate cells in the fixed utility row");
        pane.NewConversation(); pane.busy_ = true; pane.displayGroup_ = "v209-burst";
        pane.Add(L"AI", L"", "burst");
        for (int i = 0; i < 800; ++i)
            pane.events_.push_back({{"event", "textDelta"}, {"messageId", "burst"}, {"text", "0123456789"}});
        const auto started = GetTickCount64();
        pane.Poll();
        check(!pane.events_.empty(), "A single UI poll drained the whole output burst");
        while (!pane.events_.empty()) pane.Poll();
        check(pane.messages_.front().text.size() == 8000, "Sliced UI delivery lost or duplicated deltas");
        pane.collapsedGroups_.erase("v209-burst"); pane.Reflow();
        std::cout << "[TIMING] v209 800 streamed events and long-line expand " << GetTickCount64() - started << "ms\n";
        check(GetTickCount64() - started < 5000, "Bounded long-line rendering remained unresponsive");
        pane.busy_ = false; pane.NewConversation();
        pane.Add(L"AI", std::wstring(120000, L'x'), "large-final");
        pane.scroll_.offset = pane.scroll_.Maximum() / 2;
        const auto paintStarted = GetTickCount64();
        ui_test::SaveUiEvidence(pane.transcript_, L"v209-large-message-viewport.png", [&](HDC dc) { pane.PaintTranscript(dc); });
        std::cout << "[TIMING] v209 120k-character message visible-area paint " << GetTickCount64() - paintStarted << "ms\n";
        check(GetTickCount64() - paintStarted < 3000, "Visible-area painting allocated/painted the complete long card");
        pane.NewConversation();
        pane.busy_ = pane.taskAuthorized_ = true; pane.displayGroup_ = "overload-turn";
        for (int i = 0; i < 2052; ++i) pane.QueueEvent({{"event", "taskActivity"}});
        check(pane.eventOverflowed_ && pane.events_.size() == 2, "Queue overload lost its diagnostic/disconnect pair");
        pane.Poll();
        check(!pane.busy_ && !pane.taskAuthorized_ && pane.status_.find(L"缓冲上限") != std::wstring::npos,
              "Overflow left a busy but disconnected task or concealed lost display data");
        pane.eventOverflowed_ = false;
        pane.NewConversation();
        pane.busy_ = true; pane.displayGroup_ = "queued-final";
        pane.completionDeadline_ = GetTickCount64() - 1;
        for (int i = 0; i < 70; ++i) pane.QueueEvent({{"event", "status"}, {"message", "pending"}});
        pane.QueueEvent({{"event", "taskActivity"}});
        pane.Poll();
        check(pane.busy_ && !pane.events_.empty(), "Quiet completion ended a turn with queued activity");
        while (!pane.events_.empty()) pane.Poll();
        check(pane.busy_ && pane.completionDeadline_ == 0, "Queued activity failed to cancel quiet completion");
        pane.busy_ = false; pane.NewConversation();
    }
    struct WorkspaceProbe { AiPane *pane; bool dark; int dpi; std::string error; };
    static void CALLBACK WorkspaceTick(HWND parent,UINT,UINT_PTR timer,DWORD) {
        auto p=reinterpret_cast<WorkspaceProbe*>(GetPropW(parent,L"SerialCtl.TestWorkspace"));
        if(!p || !p->pane->workspaceDialog_) return;
        auto &pane=*p->pane;const HWND dialog=pane.workspaceDialog_;
        try {
            auto pending=std::find_if(pane.requests_.begin(),pane.requests_.end(),[](const auto &r) {return r.second.purpose=="directory";});
            if(pending==pane.requests_.end()) {
                // A timer can run before the modal's first WM_PAINT. The
                // production browser intentionally waits for that first form.
                DWORD_PTR paintJob = 0;
                if (GetWindowSubclass(dialog, UiFirstPaintProc, 89, &paintJob)) { UpdateWindow(dialog); return; }
                throw std::runtime_error("Workspace browser did not query existing App Server connection");
            }
            pane.Event({{"event","response"},{"token",pending->first},{"result",{{"entries",ai::Json::array({
                {{"fileName","src"},{"isDirectory",true}},{{"fileName","docs"},{"isDirectory",true}},{{"fileName","file.c"},{"isDirectory",false}}})}}}});
            auto combo=GetDlgItem(dialog,208);
            if(SendMessageW(combo,CB_GETCOUNT,0,0)!=3) throw std::runtime_error("Workspace dropdown lost actual child directories");
            RECT face{};GetClientRect(combo,&face);
            const auto arrowPoint=MAKELPARAM(face.right-MulDiv(14,p->dpi,96),face.bottom/2);
            SendMessageW(combo,WM_LBUTTONDOWN,MK_LBUTTON,arrowPoint);SendMessageW(combo,WM_LBUTTONUP,0,arrowPoint);
            if(!SendMessageW(combo,CB_GETDROPPEDSTATE,0,0)) throw std::runtime_error("Painted directory arrow does not open native dropdown at scaled DPI");
            SendMessageW(combo,CB_SHOWDROPDOWN,FALSE,0);
            wchar_t leaf[256]{};SendMessageW(combo,CB_GETLBTEXT,1,reinterpret_cast<LPARAM>(leaf));
            if(std::wstring(leaf).find(L'/')!=std::wstring::npos)
                throw std::runtime_error("Directory choices repeat full parent paths instead of child names");
            if(!GetDlgItem(dialog,213)) throw std::runtime_error("Workspace lacks full-path disclosure and horizontal viewing");
            RECT field{},up{},refresh{},client{};
            GetWindowRect(combo,&field);GetWindowRect(GetDlgItem(dialog,211),&up);GetWindowRect(GetDlgItem(dialog,212),&refresh);
            if(field.right>=up.left || up.right>=refresh.left || field.bottom-field.top < MulDiv(24,p->dpi,96))
                throw std::runtime_error("Workspace directory/up/refresh controls overlap or clip");
            GetClientRect(dialog,&client);RECT ok{};GetWindowRect(GetDlgItem(dialog,IDOK),&ok);MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&ok),2);
            if(ok.bottom>client.bottom || ok.right>client.right) throw std::runtime_error("Workspace footer escaped modal");
            ui_test::SaveUiEvidence(dialog,L"v207-workspace-"+std::wstring(p->dark?L"dark":L"light")+L"-"+std::to_wstring(p->dpi)+L"dpi.png");
        } catch(const std::exception &e) {p->error=e.what();}
        KillTimer(parent,timer);PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
    }
    static void CheckWorkspaceDialogs(AiPane &pane) {
        const auto previousCwd=pane.workspaceDirectory_;const auto previousThread=pane.thread_;
        const auto oldCount=pane.messages_.size();
        pane.pendingWorkspace_=L"/srv/candidate";pane.busy_=true;
        const auto token=pane.nextRequest_++;
        pane.requests_[token]={"workspace-start",""};
        pane.Event({{"event","response"},{"token",token},{"error","permission denied"}});
        if(pane.workspaceDirectory_!=previousCwd || pane.thread_!=previousThread || pane.messages_.size()<oldCount || pane.busy_)
            throw std::runtime_error("Failed workspace switch destroyed the previous conversation");
        const auto font=pane.font_;const int dpi=pane.dpi_;const auto dark=pane.dark_;
        for(int scale : {96,120,144,192}) for(bool theme : {false,true}) {
            auto scaled=CreateFontW(-MulDiv(15,scale,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            pane.font_=scaled;pane.dpi_=scale;pane.style_.dpi=scale;pane.style_.font=scaled;pane.Theme(theme);
            pane.directoryPending_=false;pane.busy_=false;
            WorkspaceProbe probe{&pane,theme,scale,{}};
            auto parent=GetAncestor(pane.window_,GA_ROOT);
            SetPropW(parent,L"SerialCtl.TestWorkspace",reinterpret_cast<HANDLE>(&probe));
            SetTimer(parent,9207,50,WorkspaceTick);
            const bool changed=pane.ChooseWorkspace();
            KillTimer(parent,9207);RemovePropW(parent,L"SerialCtl.TestWorkspace");
            pane.font_=font;pane.style_.font=font;DeleteObject(scaled);
            if(!probe.error.empty()) throw std::runtime_error(probe.error);
            if(changed || pane.workspaceDirectory_!=previousCwd || pane.thread_!=previousThread)
                throw std::runtime_error("Canceling workspace dialog changed the current conversation");
        }
        pane.dpi_=dpi;pane.style_.dpi=dpi;pane.Theme(dark);
    }
    static void Run(AiPane &pane) {
        // The connection form must paint before loading cached directory rows,
        // preserve an early edit, and never connect merely because it opened.
        const auto savedSettings = pane.settings_;
        const auto savedChildren = pane.directoryChildren_;
        const auto savedVerified = pane.verifiedDirectory_;
        pane.settings_.host = L"192.0.2.20";
        pane.settings_.username = L"linux";
        pane.settings_.password.clear();
        pane.settings_.workingDirectory = L"/home/linux/workspace";
        pane.verifiedDirectory_ = L"/home/linux/workspace";
        pane.directoryChildren_ = AiPane::Json::array();
        for (int i = 0; i < 500; ++i) pane.directoryChildren_.push_back("/home/linux/workspace/project-" + std::to_string(i));
        struct Form { DLGTEMPLATE d{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,0,0,0,0,270,260}; WORD menu=0,cls=0,title=0; } form;
        const auto dialog = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), &form.d,
            GetAncestor(pane.window_, GA_ROOT), AiPane::ConnectProc, reinterpret_cast<LPARAM>(&pane));
        if (!dialog || SendDlgItemMessageW(dialog, 208, CB_GETCOUNT, 0, 0) != 0)
            throw std::runtime_error("AI form populated cached directories before its first paint");
        ShowWindow(dialog, SW_SHOW); UpdateWindow(dialog);
        MSG firstPaint{};
        if (!PeekMessageW(&firstPaint, dialog, UiFirstPaintReady, UiFirstPaintReady, PM_REMOVE))
            throw std::runtime_error("AI form did not schedule its post-paint initialization");
        DispatchMessageW(&firstPaint);
        if (GetDlgItem(dialog,208) || GetDlgItem(dialog,213) || pane.client_.Running())
            throw std::runtime_error("AI connection form must keep workspace controls in the page header and never connect unsolicited");
        for (bool dark : {false, true}) {
            pane.Theme(dark);
            ui_test::SaveUiEvidence(dialog, dark ? L"dialog-ai-dark.png" : L"dialog-ai-light.png");
        }
        DestroyWindow(dialog);
        pane.settings_ = savedSettings;
        pane.directoryChildren_ = savedChildren;
        pane.verifiedDirectory_ = savedVerified;
        pane.Theme(false);
        const auto previousFocus = GetFocus();
        const auto owner = GetAncestor(pane.window_, GA_ROOT);
        ShowWindow(pane.window_, SW_HIDE); SetFocus(owner); pane.FocusInput();
        if (GetFocus() != owner) throw std::runtime_error("Hidden AI input stole focus before page layout");
        ShowWindow(pane.window_, SW_SHOW); pane.FocusInput();
        if (GetFocus() != pane.Control(10)) throw std::runtime_error("Visible AI composer did not receive focus");
        if (IsWindow(previousFocus) && IsWindowVisible(previousFocus)) SetFocus(previousFocus);
        CheckV209(pane);
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
        MSG committed{};while(PeekMessageW(&committed,nullptr,0,0,PM_REMOVE)){TranslateMessage(&committed);DispatchMessageW(&committed);}
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
        require(pane.messages_[failedTool].folded, "Running process must default to collapsed");
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
            require(!pane.messages_[0].process && pane.messages_[1].folded,
                    "New tool process did not default to collapsed");
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
            require(pane.messages_.front().folded && !pane.groupStates_["unfinished-turn"].empty(),
                    "Failed/stopped process must retain a visible collapsed outcome");
        }
        pane.NewConversation();
        pane.stopRequested_ = false;
        pane.requests_[1002] = {"resume", {}};
        pane.busy_ = true;
        pane.Event({{"event", "response"}, {"token", 1002}, {"result", {
            {"conversationId", "restored-thread"}, {"messages", {
                {{"role", "AI"}, {"text", "Checking"}, {"messageId", "restored-progress"},
                 {"group", "restored-turn"}, {"status", "completed"}, {"failedStep", true}},
                {{"role", "工具"}, {"text", "exit \x1b[31m1\x1b[0m"}, {"messageId", "server:restored-tool"},
                 {"group", "restored-turn"}, {"status", "completed"}, {"failedStep", true}},
                {{"role", "AI"}, {"text", "Diagnosis"}, {"messageId", "restored-answer"},
                 {"group", "restored-turn"}, {"status", "completed"}, {"failedStep", true}}}}}}});
        require(pane.messages_[0].folded && pane.messages_[1].hidden && !pane.messages_[2].process &&
                    pane.groupStates_["restored-turn"] == L"已完成 · 含失败记录",
                "Restoring a completed turn reopened its process or lost failed-step information");
        require(pane.messages_[1].text == L"exit 1" && pane.messages_[1].technical.find(L"\\u001b") != std::wstring::npos,
                "Restored native output leaked ANSI or discarded its raw technical evidence");
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
        CheckWorkspaceDialogs(pane);
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
        require(pane.CompanionActivity() == pet::Activity::Working, "Pet did not track accepted native tool activity");
        require(pane.busy_ && pane.nativeItems_.count("server:running"),
                "Compatibility completion interrupted an active native command");
        pane.Event({{"event", "serverTool"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:running"}, {"completed", true}, {"appendStream", true}, {"text", "exit 2: ACTUAL_FAILURE"}});
        require(pane.nativeItems_.empty(), "Completed native command stayed pending");
        require(pane.CompanionActivity() == pet::Activity::Thinking, "Pet did not leave tool activity after completion");
        size_t commandCards = 0;
        for (const auto& message : pane.messages_) if (message.item == "server:running") {
            ++commandCards;
            require(message.text == L"exit 2: ACTUAL_FAILURE\nACTUAL_PROGRESS", "Native result did not replace its progress");
        }
        require(commandCards == 1, "Native tool stream duplicated its message frame");
        pane.Event({{"event", "serverTool"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:ansi"}, {"completed", false}, {"text", "diagnose"}});
        pane.Event({{"event", "serverToolDelta"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:ansi"}, {"text", "\x1b[3"}});
        pane.Event({{"event", "serverToolDelta"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:ansi"}, {"text", "1mRED\x1b[0m"}});
        auto ansi = std::find_if(pane.messages_.begin(), pane.messages_.end(), [](const auto &m) { return m.item == "server:ansi"; });
        require(ansi != pane.messages_.end() && ansi->text == L"diagnoseRED", "Split ANSI escape leaked during streaming");
        pane.Event({{"event", "serverTool"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:ansi"}, {"completed", true}, {"appendStream", true}, {"text", "finished"}});
        ansi = std::find_if(pane.messages_.begin(), pane.messages_.end(), [](const auto &m) { return m.item == "server:ansi"; });
        require(ansi->text == L"finished\nRED" && ansi->technical.find(L"\\u001b") != std::wstring::npos,
                "Completed native output must be readable and retain original escape codes in technical details");
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
        require(pane.CompanionActivity() == pet::Activity::Done, "Pet did not reflect successful task completion");
        pane.busy_ = true;
        pane.turn_ = "next-turn";
        pane.Event({{"event", "taskFinished"},
                    {"conversationId", "fixture"},
                    {"taskId", "turn-direct"},
                    {"status", "completed"}});
        require(pane.busy_ && pane.turn_ == "next-turn", "Late completion ended the following task");
        require(pane.CompanionActivity() == pet::Activity::Done, "Stale completion changed the pet state");
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
        require(pane.CompanionActivity() == pet::Activity::Failed, "Pet celebrated a failed service call");
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
