#include "AiPane.h"
#include "HostKeyDialog.h"
#include "LocalWorkspace.h"
#include "Workspace.h"
#include "UiTestEvidence.h"
#include "UiMetrics.h"
#include <shlobj.h>
#include <chrono>
#include <cmath>
#include <gdiplus.h>
#include <iostream>
#include <fstream>
#include <filesystem>
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
            MONITORINFO monitor{sizeof(monitor)};
            RECT dialog{};
            require(GetMonitorInfoW(MonitorFromWindow(GetWindow(h, GW_OWNER), MONITOR_DEFAULTTONEAREST), &monitor) &&
                        GetWindowRect(h, &dialog) && dialog.left >= monitor.rcWork.left &&
                        dialog.top >= monitor.rcWork.top && dialog.right <= monitor.rcWork.right &&
                        dialog.bottom <= monitor.rcWork.bottom,
                    "Host identity dialog escaped its owner's monitor work area");
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
    static inline bool composerMenuCaptured = false;
    static inline bool composerSettingsOpened = false;
    static inline ULONGLONG composerMenuDeadline = 0;
    static inline std::wstring composerMenuScreenshot;
    static void CALLBACK CheckComposerSettings(HWND owner, UINT, UINT_PTR timer, DWORD) {
        KillTimer(owner, timer);
        const auto dialog = FindWindowW(L"#32770", L"连接 AI 服务");
        DWORD process = 0;
        if (dialog) GetWindowThreadProcessId(dialog, &process);
        if (process == GetCurrentProcessId() && GetDlgItem(dialog, 200) && GetDlgItem(dialog, IDCANCEL)) {
            composerSettingsOpened = true;
            try { ui_test::SaveUiEvidence(dialog, L"ai-settings-opened-from-header.png"); } catch (...) {}
            SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        } else EndMenu();
    }
    static void CALLBACK CaptureComposerMenu(HWND owner, UINT, UINT_PTR timer, DWORD) {
        HWND popup = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM output) -> BOOL {
            wchar_t name[24]{};
            GetClassNameW(window, name, static_cast<int>(std::size(name)));
            if (!IsWindowVisible(window) || wcscmp(name, L"#32768") != 0) return TRUE;
            *reinterpret_cast<HWND *>(output) = window;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&popup));
        if (!popup && GetTickCount64() < composerMenuDeadline) return;
        KillTimer(owner, timer);
        if (popup) {
            try {
                RECT popupBounds{}, anchor{};
                GetWindowRect(popup, &popupBounds);
                GetWindowRect(GetDlgItem(owner, 33), &anchor);
                const auto region = CreateRectRgn(0, 0, 0, 0);
                const bool rounded = region && GetWindowRgn(popup, region) != ERROR && !PtInRegion(region, 0, 0);
                if (region) DeleteObject(region);
                const auto dc = GetDC(owner);
                const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
                ReleaseDC(owner, dc);
                ui_test::SaveUiEvidence(popup, composerMenuScreenshot, [&](HDC dc) {
                    if (!PrintWindow(popup, dc, PW_CLIENTONLY))
                        throw std::runtime_error("Cannot capture the native popup client");
                });
                composerMenuCaptured = rounded && popupBounds.bottom <= anchor.top &&
                    popupBounds.right - popupBounds.left <= MulDiv(380, dpi, 96);
            } catch (...) {}
        }
        EndMenu();
    }
    static void RunVoiceComposer(AiPane &pane) {
        SetWindowTextW(pane.Control(10), L"排查 日志");
        SendMessageW(pane.Control(10), EM_SETSEL, 3, 3);
        pane.thread_ = "voice-fixture";
        const auto requests = pane.requests_.size();
        const auto messages = pane.messages_.size();
        wchar_t draft[64]{};
        const auto preview = pane.Control(35);
        pane.SetVoicePreview(L"正在识别\nCOM");
        RECT previewBounds{}, draftBounds{};
        GetWindowRect(preview, &previewBounds);
        GetWindowRect(pane.Control(10), &draftBounds);
        if (!IsWindowVisible(preview) || !(GetWindowLongPtrW(preview, GWL_STYLE) & ES_READONLY) ||
            previewBounds.bottom >= draftBounds.top)
            throw std::runtime_error("Live voice preview must be read-only and sit above the draft");
        pane.SetVoicePreview(L"正在识别 COM3");
        wchar_t revised[64]{};
        GetWindowTextW(preview, revised, static_cast<int>(std::size(revised)));
        GetWindowTextW(pane.Control(10), draft, static_cast<int>(std::size(draft)));
        if (std::wstring(revised) != L"正在识别 COM3" || std::wstring(draft) != L"排查 日志" ||
            pane.requests_.size() != requests || pane.messages_.size() != messages)
            throw std::runtime_error("Preview revisions must stay separate from the draft and chat");
        pane.SetVoicePreview({});
        if (IsWindowVisible(preview)) throw std::runtime_error("Cleared preview must be hidden");
        const bool inserted = pane.InsertVoiceDraft(L"COM3 ");
        GetWindowTextW(pane.Control(10), draft, static_cast<int>(std::size(draft)));
        if (!inserted || std::wstring(draft) != L"排查 COM3 日志" ||
            pane.thread_ != "voice-fixture" || pane.requests_.size() != requests || pane.messages_.size() != messages)
            throw std::runtime_error("Voice text must insert only into the editable draft");
        if (pane.InsertVoiceDraft(std::wstring(32768, L'x')))
            throw std::runtime_error("Voice insertion exceeding draft capacity should fail atomically");
        std::cout << "[PASS] Live voice preview, draft caret insertion and chat isolation\n";
    }

    static void RunReplyFormat(AiPane &pane) {
        using ai::markup::Ink;
        using ai::markup::Kind;
        const auto require = [](bool condition, const char *message) {
            if (!condition) throw std::runtime_error(message);
        };
        const std::wstring fence(3, L'`');
        struct Sample { const wchar_t *tag, *line, *token; Ink ink; };
        const Sample samples[]{
            {L"js", L"const value = 'example';", L"const", Ink::Keyword},
            {L"typescript", L"interface Device { ready: boolean }", L"interface", Ink::Keyword},
            {L"python", L"def main(): # status", L"# status", Ink::Comment},
            {L"sql", L"SeLeCt 42 FROM devices", L"SeLeCt", Ink::Keyword},
            {L"html", L"<button disabled>Ready</button>", L"disabled", Ink::Name},
            {L"css", L".result { color: red; }", L"color", Ink::Name},
            {L"java", L"record Port(int number) {}", L"record", Ink::Keyword},
            {L"csharp", L"namespace SerialCtl;", L"namespace", Ink::Keyword},
            {L"bash", L"printf '%s' ${device}", L"${device}", Ink::Name},
            {L"cpp", L"#include <stdio.h>", L"#include <stdio.h>", Ink::Name},
            {L"json", L"{\"status\": false}", L"\"status\"", Ink::Name},
        };
        for (const auto &sample : samples) {
            const auto blocks = ai::markup::Parse(fence + sample.tag + L"\n" + sample.line + L"\n" + fence);
            require(blocks.size() == 1 && blocks[0].kind == Kind::Code && blocks[0].lines.size() == 1,
                    "Language sample should be one fenced code block");
            const auto &line = blocks[0].lines.front();
            require(std::any_of(line.spans.begin(), line.spans.end(), [&](const auto &span) {
                return span.ink == sample.ink && line.value.substr(span.start, span.length) == sample.token;
            }), "Expected language token highlight");
        }
        const auto fallback = ai::markup::Parse(fence + L"unknown\nlet file = a|b");
        require(fallback.front().lines.front().spans.empty(), "Unknown language should retain plain text");
        const auto comment = ai::markup::Parse(fence + L"js\n/* start\nend */ const x = 7;\n" + fence);
        require(comment.front().lines[1].spans.front().ink == Ink::Comment &&
            comment.front().lines[1].spans.back().ink == Ink::Number, "Multiline comments should resume normal highlighting");

        const auto table = ai::markup::Parse(L"| 项目 | 结果 |\n| --- | ---: |\n| **连接** | `A|B` 与 X\\|Y |");
        require(table.size() == 1 && table.front().rows.size() == 2 &&
            table.front().rows[1][1].value == L"A|B 与 X|Y" &&
            table.front().rows[1][0].spans.front().ink == Ink::Bold &&
            table.front().rows[1][1].spans.front().ink == Ink::InlineCode &&
            table.front().alignment[1] == ai::markup::Alignment::Right, "Pipe, inline code and column alignment must survive parsing");
        require(ai::markup::Parse(L"| 项目 | 结果 |\n| --").front().kind == Kind::Prose, "Incomplete divider should display as text");
        require(ai::markup::Parse(L"| 日志 | 内容 |\n| --- | --- |", false).front().kind == Kind::Prose,
                "Tool output should retain its literal pipes");
        require(ai::markup::Parse(L"| A | B |\n| --- | --- |\n| one | two | three |").front().rows[1][1].value ==
                L"two | three", "Irregular model output must not drop trailing cell text");

        pane.Add(L"AI", L"| 项目 | 结果 |\n| --", "reply-stream");
        auto &stream = pane.messages_.back();
        require(stream.blocks.empty(), "Partial table must remain readable");
        stream.text += L"- | --- |\n| 路径 | `/srv/serialctl` |";
        pane.Reflow(true);
        require(stream.blocks.size() == 1 && stream.blocks[0].kind == Kind::Table, "A completed divider must form a table");
        const auto columns = stream.blocks[0].columns;
        stream.text += L"\n| 状态 | 新的一行包含更多中文和一个很长的诊断信息 |";
        pane.Reflow(true);
        require(stream.blocks[0].columns == columns && stream.blocks[0].rows.size() == 3, "Streamed rows must keep column geometry stable");
        pane.messages_.clear();

        pane.Add(L"AI", L"| 类别 | 内容 |\n| --- | --- |\n| 事实 | ", "partial-row1-stream");
        auto &partial = pane.messages_.back();
        require(partial.blocks.size() == 1 && partial.blocks[0].kind == Kind::Table && !partial.blocks[0].tableReady,
                "Row with an empty cell must not freeze table columns prematurely");
        partial.text += L"5";
        pane.Reflow(true);
        const int narrowContentCol = partial.blocks[0].columns[1];
        partial.text += L"396 处于 SGMII 模式且为 master，但 LINK_STAT=0 |";
        pane.Reflow(true);
        require(partial.blocks[0].columns[1] > narrowContentCol && partial.blocks[0].tableReady,
                "Streaming into the first data row must still widen its content column");
        const auto settledCols = partial.blocks[0].columns;
        partial.text += L"\n| 事实 | 对端能力字 0xEC00 不是合法单纤模式字，后续行对齐 |";
        pane.Reflow(true);
        require(partial.blocks[0].columns == settledCols && partial.blocks[0].rows.size() == 3,
                "Subsequent rows must inherit settled row-1 column geometry");
        pane.messages_.clear();

        const std::wstring code = L"\t{\r\n  \"status\": true,\r\n\r\n  \"trace\": \"" + std::wstring(128, L'x') +
                                  L"END\"\r\n}\r\n";
        const std::wstring source = L"## 诊断报告\n\n**运行环境确认**\n| 项目 | 结果 |\n| --- | --- |\n"
            L"| 程序路径 | `/home/linux/.local/bin/codex` |\n| 输入数组 | `{5, 2, 8, 1, 3}` |\n\n"
            L"**算法特点** 额外空间为 `O(1)`，平均为 `O(n²)`。\n\n"
            L"**服务返回的 JSON**\n" + fence + L"json\r\n" + code + fence;
        for (const bool dark : {false, true}) {
            pane.Theme(dark);
            for (const int width : {740, 340}) {
                SetWindowPos(pane.Handle(), nullptr, 0, 0, pane.D(width), pane.D(670), SWP_NOZORDER);
                pane.messages_.clear();
                pane.Add(L"AI", source, "reply-render");
                pane.scroll_.offset = 0;
                const auto &message = pane.messages_.back();
                require(message.blocks.size() == 2 && message.blocks[0].kind == Kind::Table &&
                        message.blocks[1].kind == Kind::Code, "Native reply should lay out one table and one code box");
                const auto &block = message.blocks[1];
                require(AiPane::ReplyCodeText(message, block) == code, "Copy must preserve the exact code payload and CRLF");
                require(block.maximum > 0, "Long code line must have local horizontal overflow");
                const auto line = std::find_if(message.paragraphs.begin(), message.paragraphs.end(),
                    [](const auto &item) { return item.text.find(L"END") != std::wstring::npos; });
                require(line != message.paragraphs.end() && line->lines.size() == 1, "Long code line must not wrap");
                const auto path = std::find_if(message.paragraphs.begin(), message.paragraphs.end(),
                    [](const auto &item) { return item.text.find(L"/home/linux") != std::wstring::npos; });
                require(path != message.paragraphs.end(), "Table path must have a measured cell");
                if (width == 740 && !path->spans.empty() && path->lines.size() == 1) {
                    const auto &sp = path->spans.front();
                    HDC checkDc = GetDC(pane.transcript_);
                    HGDIOBJ oldF = SelectObject(checkDc, pane.codeFont_);
                    SIZE rawSz{};
                    GetTextExtentPoint32W(checkDc, path->text.data() + sp.start, static_cast<int>(sp.length), &rawSz);
                    SelectObject(checkDc, oldF);
                    ReleaseDC(pane.transcript_, checkDc);
                    const int spanAdv = path->lines[0].advances[sp.start + sp.length - path->lines[0].start] -
                                        path->lines[0].advances[sp.start - path->lines[0].start];
                    require(spanAdv >= static_cast<int>(rawSz.cx) + pane.D(5),
                            "Inline code pill must reserve right-side breathing padding beyond raw glyph extent");
                }
                const POINT cell{message.bounds.left + path->lines[0].rect.left + 1,
                                 message.bounds.top + path->lines[0].rect.top + 1};
                require(std::get<1>(pane.HitText(cell)) == static_cast<size_t>(path - message.paragraphs.begin()),
                        "Table hit testing must select the cell under the pointer");
                if (width == 740) {
                    RECT viewport{};
                    GetClientRect(pane.transcript_, &viewport);
                    const auto screen = GetDC(pane.transcript_);
                    const auto memory = CreateCompatibleDC(screen);
                    const auto bitmap = CreateCompatibleBitmap(screen, viewport.right, viewport.bottom);
                    const auto previous = SelectObject(memory, bitmap);
                    pane.PaintTranscript(memory);
                    bool foundComplexity = false, boxed = false;
                    for (const auto &paragraph : message.paragraphs) {
                        if (paragraph.text.find(L"O(1)") == std::wstring::npos &&
                            paragraph.text.find(L"{5, 2, 8, 1, 3}") == std::wstring::npos) continue;
                        foundComplexity |= paragraph.text.find(L"O(1)") != std::wstring::npos;
                        for (const auto &rendered : paragraph.lines)
                            for (int x = rendered.rect.left; x < rendered.rect.right; ++x)
                                boxed |= GetPixel(memory, message.bounds.left + x, message.bounds.top +
                                    rendered.rect.bottom - pane.D(2)) == UiAiTheme(dark).accentSoft;
                    }
                    SelectObject(memory, previous);
                    DeleteObject(bitmap);
                    DeleteDC(memory);
                    ReleaseDC(pane.transcript_, screen);
                    require(foundComplexity && !boxed, "Inline expressions and table arrays must have no colored box");
                }
                const auto screenshot = std::wstring(L"ai-reply-") + (dark ? L"dark-" : L"light-") + std::to_wstring(width) + L".png";
                ui_test::SaveUiEvidence(pane.transcript_, screenshot, [&](HDC dc) { pane.PaintTranscript(dc); });
                const int vertical = pane.scroll_.offset;
                const POINT overCode{message.bounds.left + block.body.left + 1, message.bounds.top + block.body.top + 1};
                require(pane.TranscriptHorizontal(overCode, INT_MAX / 2) && block.offset == block.maximum &&
                    pane.scroll_.offset == vertical, "Horizontal scroll should stay inside the code box");
                const auto last = line->lines.front().advances.back();
                const POINT end{message.bounds.left + line->lines[0].rect.left + last - block.offset - 1,
                                message.bounds.top + line->lines[0].rect.top + 1};
                const auto hit = pane.HitText(end);
                require(std::get<1>(hit) == static_cast<size_t>(line - message.paragraphs.begin()) &&
                    std::get<2>(hit) >= line->text.size() - 1, "Code selection must follow its horizontal scroll offset");
            }
        }
        pane.messages_.clear();
        pane.Add(L"AI", L"| A | B | C | D | E | F | G | H |\n| --- | --- | --- | --- | --- | --- | --- | --- |\n"
                        L"| 1 | 2 | 3 | 4 | 5 | 6 | 7 | LAST |");
        pane.scroll_.offset = 0;
        const auto &wide = pane.messages_.back();
        const auto &tableBlock = wide.blocks.front();
        require(tableBlock.maximum > 0, "Wide table should stay inside its own horizontal viewport");
        require(pane.TranscriptHorizontal({wide.bounds.left + tableBlock.body.left + 1,
                    wide.bounds.top + tableBlock.body.top + 1}, 10000), "Wide table scroll should accept input");
        const auto &lastCell = wide.paragraphs.back();
        const POINT lastCellPoint{wide.bounds.left + lastCell.rect.left - tableBlock.offset + 1,
                                  wide.bounds.top + lastCell.rect.top + 1};
        require(std::get<1>(pane.HitText(lastCellPoint)) == wide.paragraphs.size() - 1, "Rightmost table cell must stay selectable after scrolling");

        // Phase 2 GFM, HTML table, list hanging indent, details toggle and link layout checks
        pane.messages_.clear();
        const std::wstring gfmDoc =
            L"# 主标题（H1）\n"
            L"## 二级标题（H2）\n"
            L"### 三级标题（H3）\n"
            L"1. 执行系统重启：<br>reboot<br>2. 下发配置：<br>tftpboot 0x82000000 u-boot.bin\n\n"
            L"* 一级条目\n  * 二级条目\n- [x] 已验证\n- [ ] 待排查\n\n"
            L"包含 **粗体**、*斜体*、***粗斜体***、~~删除~~、<mark>高亮</mark>、<kbd>Ctrl</kbd>+<kbd>C</kbd> 与 H<sub>2</sub>O。\n"
            L"点击跳转：[跳至结尾](#结尾测试) 或 <https://kernel.org>\n\n"
            L"> [!NOTE]\n> 注意检查板卡供电。\n\n"
            L"> 外层引用\n> > 嵌套引用\n\n"
            L"<details><summary>展开详细日志</summary>\n\n内部详情行\n\n</details>\n\n"
            L"<table><thead><tr><th>阶段</th><th>说明</th></tr></thead>"
            L"<tbody><tr><td><b>BUG-01</b><br><span style=\"color: gray;\">[高优]</span></td><td>右列内容</td></tr></tbody></table>\n\n"
            L"### 结尾测试\n完成。";
        for (const bool dark : {false, true}) {
            pane.Theme(dark);
            pane.messages_.clear();
            SetWindowPos(pane.Handle(), nullptr, 0, 0, pane.D(740), pane.D(670), SWP_NOZORDER);
            pane.Add(L"AI", gfmDoc, "gfm-render");
            pane.scroll_.offset = 0;
            auto &gfm = pane.messages_.back();
            require(!gfm.headingRules.empty() && gfm.taskChecks.size() == 2 &&
                    gfm.taskChecks[0].checked && !gfm.taskChecks[1].checked,
                    "GFM render must create H1 rule and task checkboxes");
            const auto rebootIt = std::find_if(gfm.paragraphs.begin(), gfm.paragraphs.end(),
                [](const auto &p) { return p.text == L"reboot"; });
            const auto step1It = std::find_if(gfm.paragraphs.begin(), gfm.paragraphs.end(),
                [](const auto &p) { return p.text.rfind(L"1. 执行系统重启", 0) == 0; });
            require(rebootIt != gfm.paragraphs.end() && step1It != gfm.paragraphs.end() &&
                    rebootIt->rect.left > step1It->rect.left,
                    "Ordered list <br> continuation must use hanging indent");
            const auto level2It = std::find_if(gfm.paragraphs.begin(), gfm.paragraphs.end(),
                [](const auto &p) { return p.text.find(L"二级条目") != std::wstring::npos; });
            const auto level1It = std::find_if(gfm.paragraphs.begin(), gfm.paragraphs.end(),
                [](const auto &p) { return p.text.find(L"一级条目") != std::wstring::npos; });
            require(level1It != gfm.paragraphs.end() && level2It != gfm.paragraphs.end() &&
                    level2It->rect.left > level1It->rect.left,
                    "Nested list items must indent deeper");
            const auto rightCellIt = std::find_if(gfm.paragraphs.begin(), gfm.paragraphs.end(),
                [](const auto &p) { return p.text == L"右列内容"; });
            require(rightCellIt != gfm.paragraphs.end(), "HTML table right cell must lay out");
            const POINT rightCellPt{gfm.bounds.left + rightCellIt->lines[0].rect.left + 2,
                                    gfm.bounds.top + rightCellIt->lines[0].rect.top + 2};
            require(std::get<1>(pane.HitText(rightCellPt)) == static_cast<size_t>(rightCellIt - gfm.paragraphs.begin()),
                    "Multi-line <br> HTML table left cell must keep HitText working on right cell");
            const auto linkPara = std::find_if(gfm.paragraphs.begin(), gfm.paragraphs.end(),
                [](const auto &p) { return p.text.find(L"跳至结尾") != std::wstring::npos; });
            require(linkPara != gfm.paragraphs.end() && !linkPara->links.empty(), "Anchor link must attach LinkRange");
            const auto &firstLink = linkPara->links.front();
            const int linkX = gfm.bounds.left + linkPara->lines[0].rect.left +
                (linkPara->lines[0].advances[firstLink.start] + linkPara->lines[0].advances[firstLink.start + firstLink.length]) / 2;
            const POINT linkPt{linkX, gfm.bounds.top + linkPara->lines[0].rect.top + 2};
            require(pane.HitLink(linkPt) == L"#结尾测试", "HitLink must detect anchor URL over link text");
            require(pane.HitLink({gfm.bounds.right - 2, linkPt.y}).empty(), "Blank padding after line must not hit link");
            const auto detailsIt = std::find_if(gfm.blocks.begin(), gfm.blocks.end(),
                [](const auto &b) { return b.kind == Kind::Details; });
            require(detailsIt != gfm.blocks.end() && !detailsIt->expanded, "Details block starts collapsed");
            const int collapsedHeight = gfm.measuredHeight;
            const POINT togglePt{gfm.bounds.left + detailsIt->toggle.left + pane.D(12),
                                 gfm.bounds.top + detailsIt->toggle.top + pane.D(12)};
            require(pane.TranscriptBlockPress(togglePt), "Clicking details toggle must be handled");
            const auto expandedDetailsIt = std::find_if(pane.messages_.back().blocks.begin(),
                pane.messages_.back().blocks.end(), [](const auto &b) { return b.kind == Kind::Details; });
            require(expandedDetailsIt != pane.messages_.back().blocks.end() &&
                    expandedDetailsIt->expanded && pane.messages_.back().measuredHeight > collapsedHeight,
                    "Clicking details must expand content and increase message height");
            const auto preScroll = pane.scroll_.offset;
            require(pane.ActivateLink(L"#结尾测试") && pane.scroll_.offset > preScroll,
                    "Internal #anchor link must scroll to matching heading");
            pane.scroll_.offset = 0;
            const auto screenshot = std::wstring(L"ai-reply-gfm-") + (dark ? L"dark.png" : L"light.png");
            ui_test::SaveUiEvidence(pane.transcript_, screenshot, [&](HDC dc) { pane.PaintTranscript(dc); });

            pane.messages_.clear();
            SetWindowPos(pane.Handle(), nullptr, 0, 0, pane.D(740), pane.D(940), SWP_NOZORDER);
            const std::wstring mathDoc =
                L"### 四、逻辑边界检视：为何会出现“先有鸡”的争议？（对应 $P_3$ 与连续性悖论）\n\n"
                L"上述推论在以下两个边界条件下需要进一步界定，否则推论会不完备：\n\n"
                L"**1. 语义切换到 $P_3$ （母体效应定义）时的反转**\n\n"
                L"一枚蛋由两部分构成：**内部胚胎基因型 $G_{\\text{embryo}}$** 与 **外部母体分泌结构 $M_{\\text{mother}}$**（如输卵管分泌的卵壳基质蛋白与蛋壳矿化结构）。\n\n"
                L"- 对关键卵 $e_k$ 而言：$G_{\\text{embryo}}(e_k)$ 属于鸡（$C=1$），但包裹它的蛋壳 $M_{\\text{mother}}(e_k)$ 是由“非鸡母体 $x_{k-1}$”合成的（$C=0$）。\n"
                L"- 如果有人把“鸡蛋”强行定义为“必须由基因型为鸡的母体所分泌产下的蛋”（$M_{\\text{mother}} = 1$），那么第一枚符合该定义的蛋只能由 $x_k$ 产下（即下一代卵 $e_{k+1}$），此时 $t(x_k) < t(e_{k+1})$。\n\n"
                L"**2. 连续演化的“连锁悖论（Sorites Paradox）”**\n\n"
                L"推论 2 中为了推理使用了二值阶跃假设 $C(x) \\in \\{0, 1\\}$。但在真实的演化生物学中，物种分化是群体基因频率和生殖隔离度的**连续函数 $C(x) \\in [0, 1]$**。\n\n"
                L"1. **合子决定公理**：受精卵时期 $t_{\\text{zygote}}$ 已完成整合。\n"
                L"2. **单细胞瓶颈与魏斯曼屏障（Weismann barrier）**：一个已孵化的成体（$t > t_{\\text{hatch}}$）不会在后天生命周期中发生全身性的跨物种突变。\n\n"
                L"### 总结\n\n"
                L"$$\n"
                L"\\text{广义蛋先于鸡（演化时间序）} \\;\\land\\; \\\\\n"
                L"\\text{合子基因型决定于受精而非孵化（发育遗传序）} \\implies \\\\\n"
                L"\\mathbf{\\text{先有蛋，后有鸡}}\n"
                L"$$";
            pane.Add(L"AI", mathDoc);
            pane.Layout();
            pane.scroll_.offset = 0;
            const auto &mathMsg = pane.messages_.back();
            require(!mathMsg.paragraphs.empty() && mathMsg.paragraphs[0].headingLevel == 3,
                    "Math test heading must be H3");
            const auto &h3 = mathMsg.paragraphs[0];
            const size_t p3Pos = h3.text.find(L"P3");
            require(p3Pos != std::wstring::npos && h3.text.find(L'\\') == std::wstring::npos,
                    "H3 heading $P_3$ must translate cleanly");
            HFONT pFont = nullptr, subFont = nullptr;
            for (const auto &sp : h3.spans) {
                if (sp.start <= p3Pos && p3Pos < sp.start + sp.length)
                    pFont = pane.ParagraphFont(h3.style, sp.ink, sp.flags);
                if (sp.start <= p3Pos + 1 && p3Pos + 1 < sp.start + sp.length)
                    subFont = pane.ParagraphFont(h3.style, sp.ink, sp.flags);
            }
            require(pFont == pane.h3Font_ && pFont != pane.codeFont_,
                    "Heading math variable $P$ must use heading font, not monospace code font");
            require(subFont == pane.subFont_,
                    "Heading math subscript $_3$ must use subFont_");

            const auto mathBlockIt = std::find_if(mathMsg.blocks.begin(), mathMsg.blocks.end(),
                [](const auto &b) { return b.kind == Kind::Math; });
            require(mathBlockIt != mathMsg.blocks.end(), "Display math block must be laid out");
            const size_t mathBlockIdx = static_cast<size_t>(mathBlockIt - mathMsg.blocks.begin());
            int mathTop = INT_MAX, mathBottom = INT_MIN;
            std::wstring mathCombined;
            for (const auto &mp : mathMsg.paragraphs) {
                if (mp.block != mathBlockIdx) continue;
                mathTop = std::min<int>(mathTop, mp.rect.top);
                mathBottom = std::max<int>(mathBottom, mp.rect.bottom);
                mathCombined += mp.text;
            }
            require(mathCombined.find(L"∧") != std::wstring::npos &&
                    mathCombined.find(L"⇒") != std::wstring::npos &&
                    mathCombined.find(L'\\') == std::wstring::npos,
                    "Display math block must render Unicode math operators without raw TeX backslashes");
            require(mathTop >= mathBlockIt->rect.top + pane.D(8) &&
                    mathBlockIt->rect.bottom - mathBottom >= pane.D(8),
                    "Display math block must keep top and bottom vertical padding");

            for (const auto &mp : mathMsg.paragraphs) {
                for (size_t li = 0; li + 1 < mp.lines.size(); ++li) {
                    const size_t nextStart = mp.lines[li + 1].start;
                    const wchar_t firstChar = nextStart < mp.text.size() ? mp.text[nextStart] : L'\0';
                    require(std::wstring_view(L"，。；：、！？）】》”’").find(firstChar) == std::wstring_view::npos,
                            "CJK wrapping must not leave forbidden closing/separating punctuation at line start");
                }
            }
            const auto parIt = std::find_if(mathMsg.paragraphs.begin(), mathMsg.paragraphs.end(),
                [](const auto &p) { return p.text.find(L"二值阶跃假设") != std::wstring::npos && p.lines.size() >= 2; });
            require(parIt != mathMsg.paragraphs.end(), "Sorites CJK/math paragraph must wrap into multiple lines");
            const auto &firstWrapped = parIt->lines[0];
            require(!firstWrapped.advances.empty() &&
                    firstWrapped.advances.back() >= (firstWrapped.rect.right - firstWrapped.rect.left) * 60 / 100,
                    "CJK paragraph must break cleanly near right edge instead of jumping back to an earlier ASCII space");

            const auto mathShot = std::wstring(L"ai-reply-math-") + (dark ? L"dark.png" : L"light.png");
            ui_test::SaveUiEvidence(pane.transcript_, mathShot, [&](HDC dc) { pane.PaintTranscript(dc); });
        }
        pane.messages_.clear();

        std::cout << "[PASS] Native reply markup, supported language tokens, streaming tables, code copy source, clipping and both themes\n";
    }

    static void RunComposer(AiPane &pane) {
        const auto check = [](bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); };
        CheckComposerContext(pane);
        pane.SetComposerWidth(pane.D(Ui::AiComposerMaxWidth96));
        SetWindowPos(pane.window_, nullptr, 0, 0, pane.D(720), 600, SWP_NOZORDER);
        pane.Layout();
        check(pane.composerBounds_.right - pane.composerBounds_.left == pane.D(720) &&
              pane.composerBounds_.left == 0, "Composer did not use available readable-column width");
        SetWindowPos(pane.window_, nullptr, 0, 0, pane.D(1020), 630, SWP_NOZORDER);
        pane.Layout();
        check(pane.composerBounds_.right - pane.composerBounds_.left == pane.D(Ui::AiComposerMaxWidth96) &&
              pane.composerBounds_.left == (pane.D(1020) - pane.D(Ui::AiComposerMaxWidth96)) / 2 &&
              !pane.composerExpanded_, "Wide single-line composer must remain a centered capsule");

        pane.busy_ = pane.showTurnProgress_ = true;
        pane.displayGroup_ = "centered-progress";
        pane.turnStarted_ = GetTickCount64();
        pane.Add(L"我", L"帮我检查 COM3 的串口连接");
        const auto checkProgress = [&] {
            const auto &latest = pane.messages_.back().bounds;
            check(!IsRectEmpty(&pane.waitingBounds_) && pane.waitingBounds_.top == latest.bottom + pane.D(12) &&
                  pane.scroll_.content == pane.waitingBounds_.bottom + pane.D(12),
                  "Turn progress must follow the latest visible message in the scrollable transcript");
        };
        checkProgress();
        pane.Add(L"工具", L"读取串口状态", "centered-tool");
        check(pane.messages_.back().folded, "Running tool details must be collapsed by default");
        checkProgress();
        pane.Event({{"event", "textStarted"}, {"messageId", "centered-answer"}, {"final", true}});
        pane.Event({{"event", "textDelta"}, {"messageId", "centered-answer"},
                    {"text", u8"## 串口连接诊断\nCOM3 已连接。\n\n| 项目 | 当前值 | 状态 |\n| --- | --- | --- |\n| 端口 | COM3 | 已连接 |\n| 波特率 | 115200 | 正常 |"}});
        checkProgress();
        const auto &user = pane.messages_.front().bounds;
        const auto &answer = pane.messages_.back().bounds;
        check(answer.right - answer.left == pane.D(Ui::AiReplyMaxWidth96) && answer.left > 0 &&
              answer.left > pane.composerBounds_.left && user.right > answer.right && user.right < pane.composerBounds_.right &&
              pane.waitingBounds_.left == answer.left,
              "Wide transcript must stagger the user message and AI reply inside the centered composer column");
        SetWindowPos(pane.window_, nullptr, 0, 0, pane.D(1020), pane.D(270), SWP_NOZORDER);
        pane.scroll_.Bottom();
        const auto thumb = pane.ScrollThumb();
        check(thumb.right == pane.D(1020) - pane.D(4) && thumb.left > pane.composerBounds_.right,
              "History scrollbar must sit at the outer pane edge, beyond the centered composer");
        SendMessageW(pane.transcript_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(thumb.left + 1, thumb.top + 1));
        SendMessageW(pane.transcript_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(thumb.left + 1, 0));
        SendMessageW(pane.transcript_, WM_LBUTTONUP, 0, MAKELPARAM(thumb.left + 1, 0));
        check(pane.scroll_.offset == 0, "The far-right history scrollbar must drag to the top");
        ui_test::SaveUiEvidence(pane.transcript_, L"ai-history-far-right-scrollbar.png",
            [&](HDC dc) { pane.PaintTranscript(dc); });
        RECT parentClient{};
        GetClientRect(GetParent(pane.window_), &parentClient);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(pane.window_, MONITOR_DEFAULTTONEAREST), &monitor);
        POINT origin{};
        ClientToScreen(pane.window_, &origin);
        const int captureWidth = std::min({pane.D(1020), static_cast<int>(parentClient.right),
                                           static_cast<int>(monitor.rcMonitor.right - origin.x)});
        const int captureHeight = std::min({680, static_cast<int>(parentClient.bottom),
                                             static_cast<int>(monitor.rcMonitor.bottom - origin.y)});
        SetWindowPos(pane.window_, nullptr, 0, 0, captureWidth, captureHeight, SWP_NOZORDER);
        for (bool dark : {false, true}) {
            pane.Theme(dark);
            ui_test::SaveUiEvidence(pane.window_, dark ? L"ai-centered-progress-dark.png" : L"ai-centered-progress-light.png");
        }
        SetWindowPos(pane.window_, nullptr, 0, 0, pane.D(340), 600, SWP_NOZORDER);
        check(user.right == answer.right && answer.right < pane.D(340), "Narrow reply and prompt must fit in the transcript");
        checkProgress();
        pane.Event({{"event", "taskFinished"}, {"status", "completed"}});
        check(IsRectEmpty(&pane.waitingBounds_) && pane.messages_[1].folded && !pane.messages_.back().folded,
              "Completed turns must fold the process and leave the final reply visible");
        pane.NewConversation();

        SetWindowPos(pane.window_, nullptr, 0, 0, pane.D(720), 600, SWP_NOZORDER);
        for (bool dark : {false, true}) {
            pane.Theme(dark);
            SetWindowTextW(pane.Control(10), L"");
            check(pane.currentInputHeight_ == pane.InputLineHeight(), "Capsule did not start with one line");
            ui_test::SaveUiEvidence(pane.window_, dark ? L"ai-composer-dark.png" : L"ai-composer-light.png");
            SetWindowTextW(pane.Control(10), L"1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10");
            check(pane.currentInputHeight_ == pane.InputLineHeight() * Ui::AiComposerMaxLines,
                  "Composer did not cap its visible text at six lines");
            RECT edit{}, plus{}, send{};
            GetWindowRect(pane.Control(10), &edit);
            GetWindowRect(pane.Control(33), &plus);
            GetWindowRect(pane.Control(11), &send);
            check(pane.composerExpanded_ && edit.bottom <= plus.top && edit.bottom <= send.top &&
                  edit.left < plus.right && edit.right > send.left - pane.D(40),
                  "Expanded composer still reserves full-height button columns");
            const auto before = SendMessageW(pane.Control(10), EM_GETFIRSTVISIBLELINE, 0, 0);
            SendMessageW(pane.Control(10), WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
            check(SendMessageW(pane.Control(10), EM_GETFIRSTVISIBLELINE, 0, 0) > before,
                  "Composer wheel did not scroll its hidden lines");
            check(pane.InputScrollThumb().bottom > pane.InputScrollThumb().top, "Composer overlay thumb is missing");
            ui_test::SaveUiEvidence(pane.window_, dark ? L"ai-composer-six-lines-dark.png" : L"ai-composer-six-lines-light.png");
        }
        SetWindowTextW(pane.Control(10), L"");
        const auto savedSettings = pane.settings_;
        pane.settings_ = ai::ServerSettings{};
        pane.settings_.host = L"192.0.2.55";
        pane.settings_.username = L"fixture";
        SetTimer(pane.window_, 992, 200, CheckComposerSettings);
        SendMessageW(pane.window_, WM_COMMAND, MAKEWPARAM(19, BN_CLICKED), reinterpret_cast<LPARAM>(pane.Control(19)));
        pane.settings_ = savedSettings;
        check(composerSettingsOpened && !pane.dialog_, "AI Settings button did not open and close its original dialog");
        wchar_t temp[MAX_PATH]{}, path[MAX_PATH]{};
        check(GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"sai", 0, path), "Cannot create attachment fixture");
        struct RemoveFixture { const wchar_t *path; ~RemoveFixture() { DeleteFileW(path); } } remove{path};
        {
            Gdiplus::Bitmap image(64, 48, PixelFormat32bppARGB);
            Gdiplus::Graphics graphics(&image);
            graphics.Clear(Gdiplus::Color(255, 30, 70, 150));
            const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            check(image.Save(path, &png, nullptr) == Gdiplus::Ok, "Cannot save controlled image");
        }
        const size_t characters = wcslen(path) + 2;
        const auto drop = GlobalAlloc(GHND, sizeof(DROPFILES) + characters * sizeof(wchar_t));
        check(drop != nullptr, "Cannot allocate controlled drop");
        auto *data = static_cast<DROPFILES *>(GlobalLock(drop));
        check(data != nullptr, "Cannot initialize controlled drop");
        data->pFiles = sizeof(DROPFILES); data->fWide = TRUE;
        memcpy(reinterpret_cast<char *>(data) + sizeof(DROPFILES), path, (characters - 1) * sizeof(wchar_t));
        GlobalUnlock(drop);
        SendMessageW(pane.Control(10), WM_DROPFILES, reinterpret_cast<WPARAM>(drop), 0);
        const auto deadline = GetTickCount64() + 3000;
        while (pane.attachmentLoad_ && GetTickCount64() < deadline) {
            pane.PollAttachments();
            Sleep(10);
        }
        check(pane.attachments_.size() == 1 && pane.attachments_[0].image && pane.attachments_[0].thumbnail &&
              !pane.attachments_[0].bytes->empty(), "Dropped image lost its bytes or thumbnail");
        check(ai::ValidAttachmentDirectory("/tmp/serialctl-ai.A123456789") &&
              !ai::ValidAttachmentDirectory("/tmp/serialctl-ai.A123456789/.."),
              "Attachment staging accepted an unexpected directory");
        ai::Attachment binary;
        binary.name = L"引号'与空格 $.bin";
        binary.bytes = std::make_shared<const Bytes>(Bytes{'f', 'o', 'o'});
        const auto chunk = ai::AttachmentChunkCommand("/tmp/serialctl-ai.A123456789/file-1.bin", binary, 0);
        check(chunk.at("command").at(5) == "Zm9v" &&
              chunk.at("command").at(4) == "/tmp/serialctl-ai.A123456789/file-1.bin" &&
              chunk.at("command").at(2).get<std::string>().find("base64 -d") != std::string::npos,
              "Attachment bytes or independent path arguments changed in transit");
        auto boundary = std::make_shared<Bytes>(ai::AttachmentChunkSize + 1, std::uint8_t{0});
        boundary->back() = 'f';
        binary.bytes = boundary;
        check(ai::AttachmentChunkCommand("/tmp/serialctl-ai.A123456789/file-2.bin", binary, ai::AttachmentChunkSize)
                  .at("command").at(5) == "Zg==", "Final attachment chunk lost its byte or padding");
        pane.Theme(false);
        ui_test::SaveUiEvidence(pane.window_, L"ai-composer-image-thumbnail.png");
        const auto model = pane.serverDefaultModel_;
        pane.serverDefaultModel_ = std::string(120, 'M');
        for (const bool dark : {false, true}) {
            pane.Theme(dark);
            composerMenuCaptured = false;
            composerMenuDeadline = GetTickCount64() + 2000;
            composerMenuScreenshot = dark ? L"ai-composer-plus-menu-dark.png" : L"ai-composer-plus-menu.png";
            SetTimer(pane.window_, 991, 60, CaptureComposerMenu);
            pane.ShowComposerMenu();
            KillTimer(pane.window_, 991);
            check(composerMenuCaptured, "Native upward composer menu must be rounded and keep a bounded width");
        }
        pane.serverDefaultModel_ = model;
        pane.busy_ = true;
        pane.submittedDraft_ = L"附件失败后保留的提问";
        pane.sendingAttachments_ = pane.attachments_;
        pane.attachmentUploadToken_ = 4242;
        pane.AttachmentResponse({{"token", 4242}, {"result", {{"exitCode", 1}}}});
        wchar_t restoredDraft[128]{};
        GetWindowTextW(pane.Control(10), restoredDraft, 128);
        check(!pane.busy_ && pane.attachments_.size() == 1 && std::wstring(restoredDraft) == pane.submittedDraft_,
              "Failed upload discarded the draft");
        pane.AttachmentResponse({{"token", 4242}, {"result", {{"exitCode", 0}, {"stdout", "/tmp/serialctl-ai.A123456789\n"}}}});
        check(!pane.busy_ && pane.attachmentUploadToken_ == 0, "Late upload response restarted a failed turn");
        pane.busy_ = true;
        pane.stopRequested_ = false;
        pane.sendingAttachments_ = pane.attachments_;
        pane.attachmentUploadToken_ = 4243;
        pane.Stop();
        pane.AttachmentResponse({{"token", 4243}, {"result", {{"exitCode", 0}, {"stdout", "32\n"}}}});
        check(!pane.busy_ && pane.attachments_.size() == 1 && pane.attachmentUploadToken_ == 0,
              "Stopped upload replayed work or discarded the attachment draft");
        SendMessageW(pane.window_, WM_COMMAND, MAKEWPARAM(900, BN_CLICKED), reinterpret_cast<LPARAM>(pane.Control(900)));
        check(pane.attachments_.empty(), "Attachment removal did not update the draft");
        std::cout << "COMPOSER PASS: centered stagger, trailing turn progress, native capsule, six lines, wheel, Plus menu, image drop and failed-upload draft\n";
    }
    static void CheckComposerContext(AiPane &pane) {
        const auto check = [](bool value, const char *why) { if (!value) throw std::runtime_error(why); };
        const auto savedModel = pane.serverDefaultModel_;
        const auto savedWorkspace = pane.workspaceDirectory_;
        const bool savedWsLocal = pane.workspaceIsLocal_;
        const bool savedDark = pane.dark_;
        wchar_t savedText[1024]{}; GetWindowTextW(pane.Control(12), savedText, 1024);
        RECT saved{}; GetWindowRect(pane.window_, &saved);
        MapWindowPoints(HWND_DESKTOP, GetParent(pane.window_), reinterpret_cast<POINT *>(&saved), 2);
        unsigned cases = 0;
        for (bool dark : {false, true}) {
            pane.Theme(dark);
            for (int width : {260, 420}) {
                MoveWindow(pane.window_, saved.left, saved.top, pane.D(width), 600, TRUE);
                for (const auto &name : {std::string(), std::string("m"), std::string("fixture-") + std::string(200, 'x')}) {
                    pane.serverDefaultModel_ = name;
                    const std::wstring label = name.empty() ? L"模型未获取" : L"默认 · " + std::wstring(name.begin(), name.end());
                    SetWindowTextW(pane.Control(12), label.c_str());
                    pane.UpdateActions();
                    const auto style = GetWindowLongPtrW(pane.Control(12), GWL_STYLE);
                    check((style & SS_TYPEMASK) == SS_OWNERDRAW && !(style & WS_TABSTOP), "Model capsule became an editable/tab-stop control");
                    RECT plus{}, send{}, input{};
                    GetWindowRect(pane.Control(33), &plus); GetWindowRect(pane.Control(11), &send);
                    GetWindowRect(pane.Control(10), &input);
                    check(!IsWindowVisible(pane.Control(12)) && !IsWindowVisible(pane.Control(14)) &&
                          !IsWindowVisible(pane.Control(15)) && !IsWindowVisible(pane.Control(32)),
                          "Context controls must be consolidated in the Plus menu");
                    check(IsWindowVisible(pane.Control(33)) && plus.right <= input.left && input.right <= send.left &&
                          plus.bottom - plus.top == pane.D(28), "Capsule actions overlap the draft");
                    RECT settings{}, history{}, pet{};
                    GetWindowRect(pane.Control(19), &settings);
                    GetWindowRect(pane.Control(17), &history);
                    GetWindowRect(pane.Control(30), &pet);
                    check(IsWindowVisible(pane.Control(19)) && IsWindowEnabled(pane.Control(19)) &&
                          pet.right < settings.left && settings.right < history.left,
                          "AI Settings action disappeared or overlaps neighboring actions");
                    RedrawWindow(pane.Control(19), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                    HDC settingsDc = GetDC(pane.Control(19));
                    check(settingsDc != nullptr, "Cannot capture the native Settings button");
                    unsigned settingsInk = 0;
                    const auto background = UiAiTheme(dark).surface;
                    for (int y = pane.D(6); y < pane.D(22); ++y)
                        for (int x = pane.D(6); x < pane.D(22); ++x)
                            settingsInk += GetPixel(settingsDc, x, y) != CLR_INVALID && GetPixel(settingsDc, x, y) != background;
                    ReleaseDC(pane.Control(19), settingsDc);
                    check(settingsInk > 8, "AI Settings button has no visible gear artwork");
                    check(pane.modelTooltip_ == (name.empty() ? L"模型未获取" : std::wstring(name.begin(), name.end())),
                          "Read-only context lost the full server model");
                    const auto image = std::wstring(L"v218-model-") + (dark ? L"dark-" : L"light-") +
                        std::to_wstring(width) + (name.empty() ? L"-unknown.png" : name == "m" ? L"-short.png" : L"-long.png");
                    ui_test::SaveUiEvidence(pane.window_, image);
                    ++cases;
                }
                int shortWidth=0,longWidth=0;
                pane.workspaceIsLocal_=false;
                for(bool longName:{false,true}) {
                    pane.workspaceDirectory_=L"/srv/"+(longName?std::wstring(200,L'x'):std::wstring(L"m"));
                    pane.UpdateActions();
                    RECT workspace{},pet{};
                    GetWindowRect(pane.Control(31),&workspace);GetWindowRect(pane.Control(30),&pet);
                    check(workspace.bottom-workspace.top==pane.D(28)&&workspace.right<=pet.left-pane.D(12),
                          "Workspace capsule overlaps header actions or changed row height");
                    check(pane.workspaceTooltip_.find(pane.workspaceDirectory_)==0,
                          "Adaptive workspace capsule lost its full-path tooltip");
                    (longName?longWidth:shortWidth)=workspace.right-workspace.left;
                    ui_test::SaveUiEvidence(pane.window_,L"v219-workspace-capsule-"+std::wstring(dark?L"dark-":L"light-")+
                        std::to_wstring(width)+(longName?L"-long.png":L"-short.png"));
                }
                check(shortWidth<longWidth,"Short workspace capsule still consumes the full available width");
            }
        }
        pane.serverDefaultModel_ = savedModel;
        pane.workspaceDirectory_ = savedWorkspace;
        pane.workspaceIsLocal_ = savedWsLocal;
        SetWindowTextW(pane.Control(12), savedText);
        pane.Theme(savedDark);
        MoveWindow(pane.window_, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top, TRUE);
        pane.UpdateActions();
        std::cout << "COMPOSER_CONTEXT_CASES=" << cases << " PASS: empty/short/long model, both palettes, narrow/normal, readonly, tooltip and composer corner\n";
    }
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
        pane.busy_ = true; pane.showTurnProgress_ = true; pane.displayGroup_ = "v209-tools";
        const ai::Json result{{"sessionId", "session-1"}, {"text", u8"当前屏幕第一行\n当前屏幕第二行"},
            {"cursor", 8490}, {"alternateScreen", true}, {"truncated", false}};
        pane.AddTool("serialctl_screen", {{"sessionId", "session-1"}}, result, L"SSH");
        check(pane.messages_.front().folded && pane.waitingBounds_.top > pane.messages_.front().bounds.bottom,
              "Turn progress must follow the collapsed tool process");
        pane.collapsedGroups_.erase("v209-tools");
        pane.userExpandedGroups_.insert("v209-tools");
        pane.Reflow();
        check(!pane.messages_.front().folded && pane.waitingBounds_.top > pane.messages_.front().bounds.bottom,
              "Turn progress must follow the visible tool process");
        check(pane.messages_.front().text.find(L"sessionId") == std::wstring::npos &&
              pane.messages_.front().text.find(L"第一行\n当前屏幕第二行") != std::wstring::npos &&
              pane.messages_.front().technical.find(L"sessionId") != std::wstring::npos,
              "Readable tool text and optional technical envelope were not separated");
        for (bool dark : {false, true}) {
            pane.Theme(dark);
            pane.Reflow();
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
        GetWindowRect(pane.Control(20), &latest); GetWindowRect(pane.Control(33), &observe);
        check(IsWindowVisible(pane.Control(33)) && !IsWindowVisible(pane.Control(32)) && !IsWindowVisible(pane.Control(16)) &&
              !IntersectRect(&overlap,&latest,&observe) && latest.bottom <= observe.top,
              "Return-to-latest action must stay above the capsule and its Plus menu");
        pane.NewConversation(); pane.busy_ = true; pane.displayGroup_ = "v209-burst";
        pane.Add(L"AI", L"", "burst");
        for (int i = 0; i < 800; ++i)
            pane.events_.push_back({{"event", "textDelta"}, {"messageId", "burst"}, {"text", "0123456789"}});
        const auto started = GetTickCount64();
        pane.Poll();
        check(!pane.events_.empty(), "A single UI poll drained the whole output burst");
        while (!pane.events_.empty()) pane.Poll();
        check(pane.messages_.front().text.size() == 8000, "Sliced UI delivery lost or duplicated deltas");
        pane.Reflow();
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
            // Issue #53: Selecting a subdirectory navigates into that level and keeps the dropdown open for continuous multi-level navigation.
            SendMessageW(combo,CB_SETCURSEL,1,0);
            SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(208,CBN_SELENDOK),reinterpret_cast<LPARAM>(combo));
            MSG dirMsg{};
            while(PeekMessageW(&dirMsg,dialog,WM_APP + 196,WM_APP + 196,PM_REMOVE)) DispatchMessageW(&dirMsg);
            auto subPending=std::find_if(pane.requests_.begin(),pane.requests_.end(),[](const auto &r){return r.second.purpose=="directory";});
            if(subPending==pane.requests_.end()) throw std::runtime_error("Selecting a subdirectory did not initiate next-level directory query");
            pane.Event({{"event","response"},{"token",subPending->first},{"result",{{"entries",ai::Json::array({
                {{"fileName","module"},{"isDirectory",true}},{{"fileName","util"},{"isDirectory",true}}})}}}});
            wchar_t updatedPath[512]{};GetDlgItemTextW(dialog,213,updatedPath,512);
            const std::wstring expectedSuffix = L"/" + std::wstring(leaf);
            if(std::wstring(updatedPath).size() < expectedSuffix.size() ||
               std::wstring(updatedPath).rfind(expectedSuffix) != std::wstring(updatedPath).size() - expectedSuffix.size())
                throw std::runtime_error("Multi-level navigation did not update full path display");
            if(!SendMessageW(combo,CB_GETDROPPEDSTATE,0,0) || SendMessageW(combo,CB_GETCOUNT,0,0)!=3)
                throw std::runtime_error("Directory selector collapsed instead of staying expanded for continuous multi-level navigation");
            SendMessageW(combo,CB_SHOWDROPDOWN,FALSE,0);
            if(!GetDlgItem(dialog,213)) throw std::runtime_error("Workspace lacks full-path disclosure and horizontal viewing");
            RECT field{},path{},up{},refresh{},client{};
            GetWindowRect(combo,&field);GetWindowRect(GetDlgItem(dialog,211),&up);GetWindowRect(GetDlgItem(dialog,212),&refresh);
            GetWindowRect(GetDlgItem(dialog,213),&path);
            if(field.right>=up.left || up.right>=refresh.left || field.bottom-field.top < MulDiv(24,p->dpi,96))
                throw std::runtime_error("Workspace directory/up/refresh controls overlap or clip");
            if(field.bottom-field.top!=path.bottom-path.top || field.bottom-field.top!=up.bottom-up.top ||
                field.bottom-field.top!=refresh.bottom-refresh.top)
                throw std::runtime_error("Workspace visible path/combo/button heights are inconsistent");
            if (reinterpret_cast<UINT_PTR>(GetPropW(GetDlgItem(dialog,212),UiButtonGlyphProperty)) !=
                    static_cast<UINT_PTR>(UiGlyph::Refresh) + 1)
                throw std::runtime_error("Workspace refresh lost shared icon or its native command control");
            GetClientRect(dialog,&client);RECT ok{};GetWindowRect(GetDlgItem(dialog,IDOK),&ok);MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&ok),2);
            if(ok.bottom>client.bottom || ok.right>client.right) throw std::runtime_error("Workspace footer escaped modal");
            ui_test::SaveUiEvidence(dialog,L"v207-workspace-"+std::wstring(p->dark?L"dark":L"light")+L"-"+std::to_wstring(p->dpi)+L"dpi.png");
        } catch(const std::exception &e) {p->error=e.what();}
        KillTimer(parent,timer);PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
    }
    static void CheckWorkspaceDialogs(AiPane &pane) {
        struct WsFixtureGuard {
            AiPane &pane;
            ai::ServerSettings settings;
            bool wsLocal;
            std::wstring localWs, remoteWs, wsDir;
            std::string sshId;
            ~WsFixtureGuard() {
                pane.settings_ = settings;
                pane.workspaceIsLocal_ = wsLocal;
                pane.localWorkspaceDirectory_ = localWs;
                pane.remoteWorkspaceDirectory_ = remoteWs;
                pane.remoteSshSessionId_ = sshId;
                pane.workspaceDirectory_ = wsDir;
                pane.SyncLocalWorkspaceService();
            }
        } wsGuard{pane, pane.settings_, pane.workspaceIsLocal_, pane.localWorkspaceDirectory_,
                  pane.remoteWorkspaceDirectory_, pane.workspaceDirectory_, pane.remoteSshSessionId_};
        pane.settings_.localMode = false;
        pane.settings_.provider = ai::AiProvider::Codex;
        pane.workspaceIsLocal_ = false;
        pane.remoteSshSessionId_.clear();
        pane.SyncLocalWorkspaceService();
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
    static void AuditLayoutLoad(AiPane &pane) {
        // Controlled worst-case expanded history; no SSH/model/network claim.
        ai::Json rows=ai::Json::array();
        auto elapsed=[](auto begin) {
            return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        };
        for (int count : {128,1000,2000}) {
            pane.NewConversation();
            SetWindowPos(pane.Handle(),nullptr,0,0,1000,650,SWP_NOZORDER|SWP_SHOWWINDOW);
            pane.deferReflow_=true;
            for (int i=0;i<count;++i) pane.Add(L"AI",std::wstring(1000,L'x'),"layout-audit-"+std::to_string(i));
            pane.deferReflow_=pane.reflowPending_=pane.appendPending_=false;
            auto begin=std::chrono::steady_clock::now();pane.Reflow();const auto cold=elapsed(begin);
            begin=std::chrono::steady_clock::now();pane.Reflow();const auto warm=elapsed(begin);
            begin=std::chrono::steady_clock::now();
            SetWindowPos(pane.Handle(),nullptr,0,0,700,650,SWP_NOZORDER);
            const auto resize=elapsed(begin);
            if (pane.messages_.size()!=static_cast<size_t>(count)) throw std::runtime_error("Layout audit input exceeded real display limit");
            const ai::Json row{{"messages",count},{"utf16_bytes",pane.messageBytes_},{"initial_reflow_ms",cold},
                {"same_width_reflow_ms",warm},{"native_resize_1000_to_700_ms",resize}};
            rows.push_back(row);std::cout<<"AI_LAYOUT_AUDIT "<<row.dump()<<'\n';
        }
        wchar_t directory[32768]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR",directory,32768))
            std::ofstream(std::filesystem::path(directory)/L"ai-expanded-history-layout.json")<<ai::Json{
                {"scope","Controlled native AI pane, expanded history; synchronous layout time, not actual mouse latency"},
                {"rows",rows}}.dump(2);
        pane.NewConversation();
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
        CheckComposerContext(pane);
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
                SetWindowPos(pane.Handle(), nullptr, 0, 0, pane.D(width), 600, SWP_NOZORDER);
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
                require(!IsWindowVisible(pane.Control(14)) &&
                        (pane.composerExpanded_ ? input.bottom <= send.top : input.right <= send.left),
                        "Composer text overlaps Send at narrow width");
                require(pane.scroll_.offset == 0, "Theme/resize must preserve the history reading position");
                UpdateWindow(pane.Handle());
                UpdateWindow(pane.transcript_);
            }
        }
        // Issue #48: Auto-expanding input height upward, clamped to maxInputHeight, shrinking back on clear.
        {
            SetWindowPos(pane.Handle(), nullptr, 0, 0, pane.D(600), 600, SWP_NOZORDER);
            SetWindowTextW(pane.Control(10), L"");
            pane.Layout();
            RECT baseInput{}, baseSend{}, baseCanvas{};
            GetWindowRect(pane.Control(10), &baseInput);
            GetWindowRect(pane.Control(11), &baseSend);
            GetWindowRect(pane.transcript_, &baseCanvas);
            require(baseInput.bottom - baseInput.top == pane.InputLineHeight(), "Empty input must show one measured line");

            SetWindowTextW(pane.Control(10), L"第1行日志\r\n第2行日志\r\n第3行日志\r\n第4行日志\r\n第5行日志");
            RECT midInput{}, midSend{}, midCanvas{};
            GetWindowRect(pane.Control(10), &midInput);
            GetWindowRect(pane.Control(11), &midSend);
            GetWindowRect(pane.transcript_, &midCanvas);
            require(midInput.bottom - midInput.top > pane.InputLineHeight() && midInput.top < baseInput.top,
                    "Multi-line input did not expand upward");
            require(midSend.top == baseSend.top && midSend.bottom == baseSend.bottom,
                    "Expanding input moved the bottom action row");
            require(midCanvas.bottom <= midInput.top,
                    "Transcript overlapped the expanded composer");

            std::wstring thirtyLines;
            for (int i = 0; i < 30; ++i) thirtyLines += L"异常堆栈行 " + std::to_wstring(i) + L"\r\n";
            SetWindowTextW(pane.Control(10), thirtyLines.c_str());
            RECT maxInput{};
            GetWindowRect(pane.Control(10), &maxInput);
            require(maxInput.bottom - maxInput.top <= pane.InputLineHeight() * Ui::AiComposerMaxLines && maxInput.bottom - maxInput.top >= midInput.bottom - midInput.top,
                    "Long input exceeded maxInputHeight cap");

            SetWindowTextW(pane.Control(10), L"");
            RECT clearedInput{};
            GetWindowRect(pane.Control(10), &clearedInput);
            require(clearedInput.bottom - clearedInput.top == pane.InputLineHeight() && clearedInput.top == baseInput.top,
                    "Clearing input did not shrink composer back to baseline height");
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
        RECT sendArea{};
        GetWindowRect(pane.Control(11), &sendArea);
        require(IsWindowVisible(pane.Control(11)) && !IsWindowVisible(pane.Control(21)) &&
                    sendArea.right > sendArea.left && sendArea.bottom > sendArea.top,
                "Busy Stop must reuse the visible Send button; legacy Stop remains hidden");
        pane.sendGesture_ = 0; // A press that started as Send must not cancel a just-started turn.
        pane.Action(11);
        require(!pane.stopRequested_, "Send press crossed into Stop after the task started");
        SendMessageW(pane.Control(11), WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(5,5));
        pane.Action(11);
        require(!pane.stopRequested_, "Send double-click cancelled the active task");
        SendMessageW(pane.Control(11), WM_LBUTTONUP, 0, MAKELPARAM(5,5));
        require(pane.sendGesture_ == -1, "Double-click suppression leaked into the next gesture");
        pane.sendGesture_ = 1; // A fresh press while busy is the explicit Stop action.
        pane.Action(11);
        require(pane.stopRequested_, "Shared Send/Stop button did not request cancellation");
        require(!IsWindowEnabled(pane.Control(11)), "Stopping button must reject repeated cancellation");
        for (int i = 0; i < 5; ++i) pane.Action(11);
        require(pane.stopRequested_, "Repeated Stop lost the active cancellation state");
        pane.sendGesture_ = -1;
        pane.busy_ = pane.stopRequested_ = false;
        pane.NewConversation();
        pane.displayGroup_ = "process-fixture";
        pane.thread_ = "process-thread";
        pane.busy_ = true;
        pane.showTurnProgress_ = true;
        pane.Add(L"我", L"执行诊断");
        require(pane.waitingBounds_.top > pane.messages_.front().bounds.bottom,
                "Turn progress must appear after the user message");
        pane.Add(L"工具", L"实际执行输出", "process-tool");
        pane.Add(L"AI", L"最终回答", "process-final");
        pane.Event({{"event", "taskFinished"}, {"conversationId", "process-thread"}, {"status", "completed"}});
        require(!pane.showTurnProgress_ && pane.messages_[1].folded && !pane.messages_[2].folded,
                "Successful process must fold without hiding final answer");
        require(pane.messages_[1].text == L"实际执行输出", "Folding discarded original tool evidence");
        {
            RECT size{};
            GetClientRect(pane.transcript_, &size);
            auto screen = GetDC(pane.transcript_), dc = CreateCompatibleDC(screen);
            auto bitmap = CreateCompatibleBitmap(screen, size.right, size.bottom);
            auto old = SelectObject(dc, bitmap);
            pane.PaintTranscript(dc);
            const auto summaryPixel = GetPixel(dc, pane.messages_[1].bounds.left + pane.D(20),
                                               pane.messages_[1].bounds.top + pane.D(34) - pane.scroll_.offset);
            SelectObject(dc, old);
            DeleteObject(bitmap);
            DeleteDC(dc);
            ReleaseDC(pane.transcript_, screen);
            require(summaryPixel == UiTheme(pane.dark_).surface,
                    "Collapsed process summary must remain visibly painted and expandable");
        }

        pane.collapsedGroups_.erase("process-fixture");
        pane.userExpandedGroups_.insert("process-fixture");
        pane.Reflow();
        require(!pane.messages_[1].folded, "Process could not be expanded");
        pane.displayGroup_ = "failed-fixture";
        pane.busy_ = true;
        pane.groupStates_[pane.displayGroup_] = L"含失败记录";
        pane.Add(L"工具", L"退出码 1", "failed-tool");
        const auto failedTool = pane.messages_.size() - 1;
        pane.Add(L"AI", L"已根据日志完成分析", "recovered-final");
        require(pane.messages_[failedTool].folded, "Running failed process must default to collapsed");
        pane.Event({{"event", "taskFinished"}, {"status", "completed"}});
        require(pane.messages_[failedTool].folded && !pane.messages_.back().folded &&
                    pane.groupStates_["failed-fixture"] == L"已完成 · 含失败记录",
                "A completed answer must keep diagnostic failures collapsed while retaining their summary");
        require(pane.messages_[failedTool].text == L"退出码 1", "Folding lost a failed command's evidence");
        pane.displayGroup_ = "manual-expand-fixture";
        pane.busy_ = true;
        pane.Add(L"工具", L"步骤一", "manual-step-1");
        const auto manualHeader = pane.messages_.size() - 1;
        require(pane.messages_[manualHeader].folded, "Running step must start collapsed");
        {
            const auto &hdr = pane.messages_[manualHeader].bounds;
            const auto clickHdr = MAKELPARAM(hdr.right - pane.D(20), hdr.top + pane.D(20) - pane.scroll_.offset);
            SendMessageW(pane.transcript_, WM_LBUTTONDOWN, MK_LBUTTON, clickHdr);
            SendMessageW(pane.transcript_, WM_LBUTTONUP, 0, clickHdr);
        }
        require(!pane.messages_[manualHeader].folded, "Clicking header must expand running process");
        pane.Add(L"AI", L"手动展开后完成回答", "manual-final");
        pane.Event({{"event", "taskFinished"}, {"status", "completed"}});
        require(!pane.messages_[manualHeader].folded && !pane.messages_.back().folded,
                "User-expanded process group must remain expanded after completion");
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
                    "New tool process should default to collapsed while running");
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
        pane.effectiveAccess_ = L"Full Access";
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
        require(pane.effectiveAccess_ == L"待验证" &&
                    pane.workspaceTooltip_.find(L"请求权限：Full Access\n实际权限：待验证") != std::wstring::npos,
                "Resume without permission metadata retained a previous Full Access result");
        pane.requests_[1010] = {"turn", {}};
        pane.Event({{"event", "response"}, {"token", 1010},
                    {"result", {{"taskId", "restricted-turn"}, {"effectiveSandbox", "readOnly"}}}});
        require(pane.effectiveAccess_ == L"受限 (read-only)", "Server restriction was not reflected");
        pane.requests_[1011] = {"turn", {}};
        pane.Event({{"event", "response"}, {"token", 1011}, {"result", {{"taskId", "unknown-turn"}}}});
        require(pane.effectiveAccess_ == L"待验证", "A turn without permission metadata retained stale permission");
        pane.effectiveAccess_ = L"Full Access";
        pane.requests_[1012] = {"turn", {}};
        pane.Event({{"event", "response"}, {"token", 1012}, {"error", "permission denied for requested sandbox"}});
        require(pane.effectiveAccess_ == L"待验证" &&
                    pane.workspaceTooltip_.find(L"实际权限：待验证") != std::wstring::npos,
                "A rejected turn must not retain a previously confirmed Full Access label");
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
                    const auto background = GetPixel(dc, message.bounds.left + paragraph.rect.right - pane.D(8),
                                                     message.bounds.top + paragraph.rect.top + pane.D(8) - pane.scroll_.offset);
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
            require(message.text == L"exit 2: ACTUAL_FAILURE\n\n**输出**\n```text\nACTUAL_PROGRESS\n```",
                    "Native result did not replace its progress");
        }
        require(commandCards == 1, "Native tool stream duplicated its message frame");
        pane.Event({{"event", "serverTool"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:ansi"}, {"completed", false}, {"text", "diagnose"}});
        pane.Event({{"event", "serverToolDelta"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:ansi"}, {"text", "\x1b[3"}});
        pane.Event({{"event", "serverToolDelta"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:ansi"}, {"text", "1mRED\x1b[0m"}});
        auto ansi = std::find_if(pane.messages_.begin(), pane.messages_.end(), [](const auto &m) { return m.item == "server:ansi"; });
        require(ansi != pane.messages_.end() && ansi->text == L"diagnose\n\n**实时输出**\n```text\nRED\n```",
                "Split ANSI escape leaked during streaming");
        pane.Event({{"event", "serverTool"}, {"conversationId", "fixture"}, {"taskId", "turn-direct"},
                    {"messageId", "server:ansi"}, {"completed", true}, {"appendStream", true}, {"text", "finished"}});
        ansi = std::find_if(pane.messages_.begin(), pane.messages_.end(), [](const auto &m) { return m.item == "server:ansi"; });
        require(ansi->text == L"finished\n\n**输出**\n```text\nRED\n```" &&
                    ansi->technical.find(L"\\u001b") != std::wstring::npos,
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

        // Decoupled 2x2 workspace mode, offline switch UI, and async mcp_workspace_* execution
        {
            const auto origWs = ai::LoadWorkspaceSettings(&pane.settings_);
            const auto origSettings = pane.settings_;
            wchar_t tmpRoot[MAX_PATH]{};
            const DWORD tn = GetTempPathW(MAX_PATH, tmpRoot);
            require(tn > 0 && tn < MAX_PATH, "GetTempPathW failed for AI pane workspace test");
            const std::wstring localWsDir =
                std::wstring(tmpRoot, tn) + L"serialctl-aipane-ws-" + std::to_wstring(GetCurrentProcessId());
            std::error_code ec;
            std::filesystem::remove_all(localWsDir, ec);
            require(std::filesystem::create_directories(localWsDir, ec), "Failed to create AI pane local ws dir");

            try {
                // Remote AI + Local Windows Workspace keeps POSIX cwd for SSH process while exposing local tools
                pane.settings_.localMode = false;
                pane.ApplyWorkspaceSelection(true, false, localWsDir, L"/srv/project-keep", "ssh-7");
                require(pane.workspaceIsLocal_ && pane.localWorkspaceDirectory_ == localWsDir &&
                            pane.remoteWorkspaceDirectory_ == L"/srv/project-keep",
                        "ApplyWorkspaceSelection failed to retain independent local and remote paths");
                require(ai::ValidRemoteWorkspacePath(pane.ProcessCwdForAi()),
                        "Remote AI with local workspace must pass POSIX cwd rather than Windows path to SSH server");
                wchar_t wsBtnText[256]{};
                GetWindowTextW(pane.Control(31), wsBtnText, 256);
                require(std::wstring(wsBtnText).find(L"本地") != std::wstring::npos,
                        "Workspace header capsule did not show local indicator");

                // Async mcp_workspace_write_file -> WM_APP+195 delivery -> real file & tool card
                pane.NewConversation();
                pane.thread_ = "fixture";
                pane.busy_ = true;
                pane.taskAuthorized_ = true;
                pane.turn_ = "turn-ws";
                const auto msgBefore = pane.messages_.size();
                ai::Json wsCall = {
                    {"event", "serialToolRequest"},
                    {"requestId", 951},
                    {"conversationId", "fixture"},
                    {"taskId", "turn-ws"},
                    {"callId", "ws-write-1"},
                    {"tool", "mcp_workspace_write_file"},
                    {"arguments", {{"path", "probe/hello.txt"}, {"content", "WIN_LOCAL_TOOL_OK"}}}};
                pane.Event(wsCall);
                const auto asyncDeadline = GetTickCount64() + 5000;
                while (pane.messages_.size() == msgBefore && GetTickCount64() < asyncDeadline) {
                    MSG m{};
                    while (PeekMessageW(&m, pane.Handle(), WM_APP + 195, WM_APP + 195, PM_REMOVE))
                        DispatchMessageW(&m);
                    pane.ProcessAsyncWork();
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                }
                require(pane.messages_.size() > msgBefore &&
                            std::filesystem::exists(std::filesystem::path(localWsDir) / L"probe" / L"hello.txt"),
                        "Async mcp_workspace_write_file did not execute off-thread and deliver result card");

                // Remote workspace must reject mcp_workspace_* access to local drive
                pane.ApplyWorkspaceSelection(false, false, localWsDir, L"/srv/project-keep", "ssh-7");
                const auto deniedBefore = pane.messages_.size();
                wsCall["requestId"] = 952;
                wsCall["callId"] = "ws-write-denied";
                wsCall["arguments"]["path"] = "probe/denied.txt";
                pane.Event(wsCall);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                require(!std::filesystem::exists(std::filesystem::path(localWsDir) / L"probe" / L"denied.txt") &&
                            pane.messages_.size() == deniedBefore,
                        "Remote workspace allowed mcp_workspace_* write");

                pane.busy_ = false;
                pane.taskAuthorized_ = false;
                pane.client_.Shutdown();
                pane.UpdateActions();
                require(IsWindowEnabled(pane.Control(31)) != FALSE,
                        "Workspace entry button must remain enabled even when AI is offline");

                // Decoupled WorkspaceProc segmented switch & controls visibility in both palettes
                pane.ApplyWorkspaceSelection(false, false, localWsDir, L"/srv/project-keep", "");
                for (bool darkTheme : {false, true}) {
                    pane.Theme(darkTheme);
                    struct WsForm { DLGTEMPLATE d{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,0,0,0,0,456,332}; WORD menu=0,cls=0,title=0; } form;
                    const HWND dlg = CreateDialogIndirectParamW(
                        GetModuleHandleW(nullptr), &form.d, GetAncestor(pane.window_, GA_ROOT),
                        AiPane::WorkspaceProc, reinterpret_cast<LPARAM>(&pane));
                    require(dlg != nullptr, "Failed to create WorkspaceProc dialog");
                    ShowWindow(dlg, SW_SHOW);
                    UpdateWindow(dlg);
                    MSG m{};
                    while (PeekMessageW(&m, dlg, WM_APP + 89, WM_APP + 95, PM_REMOVE)) DispatchMessageW(&m);

                    // Initially remote mode: SshCombo visible, BrowseLocal & LocalReadOnly hidden
                    require(IsWindowVisible(GetDlgItem(dlg, 235)) != FALSE &&
                                IsWindowVisible(GetDlgItem(dlg, 232)) == FALSE &&
                                IsWindowVisible(GetDlgItem(dlg, 233)) == FALSE,
                            "Workspace dialog initial remote mode control visibility mismatch");
                    // Switch to local mode via WsModeLocal (231)
                    SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(231, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dlg, 231)));
                    require(pane.workspaceDialogIsLocal_ &&
                                IsWindowVisible(GetDlgItem(dlg, 232)) != FALSE &&
                                IsWindowVisible(GetDlgItem(dlg, 233)) != FALSE &&
                                IsWindowVisible(GetDlgItem(dlg, 235)) == FALSE,
                            "Switching to Local workspace tab did not toggle browse/read-only controls");
                    RECT rRemote{}, rLocal{}, rPath{}, rBrowse{};
                    GetWindowRect(GetDlgItem(dlg, 230), &rRemote);
                    GetWindowRect(GetDlgItem(dlg, 231), &rLocal);
                    GetWindowRect(GetDlgItem(dlg, 213), &rPath);
                    GetWindowRect(GetDlgItem(dlg, 232), &rBrowse);
                    require(rRemote.right < rLocal.left && rPath.right < rBrowse.left &&
                                (rRemote.bottom - rRemote.top) == (rPath.bottom - rPath.top) &&
                                (rBrowse.bottom - rBrowse.top) == (rPath.bottom - rPath.top),
                            "Local workspace segmented switch / path / browse controls misaligned or overlapping");
                    ui_test::SaveUiEvidence(dlg, darkTheme ? L"v210-workspace-local-dark.png" : L"v210-workspace-local-light.png");

                    // Switch back to Remote (230) preserves both paths
                    SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(230, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dlg, 230)));
                    require(!pane.workspaceDialogIsLocal_ &&
                                IsWindowVisible(GetDlgItem(dlg, 235)) != FALSE &&
                                IsWindowVisible(GetDlgItem(dlg, 232)) == FALSE,
                            "Switching back to Remote workspace tab did not restore SSH selector");
                    DestroyWindow(dlg);
                }

                // Remote Antigravity ("ai-settings") listing, "." $HOME fallback, combo refresh on mode switches, and stale seq guard
                {
                    struct FsMockGuard {
                        AiPane &pane;
                        AiPane::RemoteFsRunner runner;
                        AiPane::DeviceRequest devices;
                        std::wstring serverHome;
                        ~FsMockGuard() {
                            pane.SetRemoteFsRunner(std::move(runner));
                            pane.devices_ = std::move(devices);
                            pane.serverHome_ = std::move(serverHome);
                        }
                    } fsGuard{pane, pane.remoteFsRunner_, pane.devices_, pane.serverHome_};

                    auto pumpDlgAndAsync = [&](int maxMs = 1500) {
                        const auto until = GetTickCount64() + static_cast<ULONGLONG>(maxMs);
                        MSG m{};
                        do {
                            bool had = false;
                            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                                had = true;
                                TranslateMessage(&m);
                                DispatchMessageW(&m);
                            }
                            pane.ProcessAsyncWork();
                            if (!pane.directoryPending_ && !had) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        } while (GetTickCount64() < until);
                    };
                    auto readComboItems = [&](HWND combo) {
                        std::vector<std::wstring> items;
                        const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
                        for (int i = 0; i < count; ++i) {
                            const int len = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, i, 0));
                            std::wstring s(len > 0 ? static_cast<size_t>(len) + 1 : 1, L'\0');
                            const int got = len > 0
                                ? static_cast<int>(SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(s.data())))
                                : 0;
                            s.resize(got > 0 ? static_cast<size_t>(got) : 0);
                            items.push_back(std::move(s));
                        }
                        return items;
                    };

                    pane.client_.Shutdown();
                    pane.settings_.localMode = false;
                    pane.settings_.provider = ai::AiProvider::Antigravity;
                    pane.settings_.host = L"10.0.0.8";
                    pane.settings_.port = 22;
                    pane.settings_.username = L"dev";
                    pane.serverHome_.clear();
                    pane.devices_ = [&](const std::string &op, const ai::Json &args) {
                        if (op == "remote_fs.sessions") return ai::Json::array();
                        return fsGuard.devices ? fsGuard.devices(op, args) : ai::Json(nullptr);
                    };

                    std::vector<std::pair<std::string, std::string>> fsCalls;
                    pane.SetRemoteFsRunner([&](const std::string &op, const ai::Json &args, AiPane::RemoteFsCallback cb) {
                        if (op != "list") return;
                        const auto sid = args.value("sessionId", "");
                        const auto path = args.value("path", "");
                        fsCalls.emplace_back(sid, path);
                        if (path == "/home/linux/workspace") {
                            cb({{"error", "No such directory"}});
                        } else if (path == ".") {
                            cb({{"ok", true},
                                {"sessionId", sid},
                                {"path", "/home/dev"},
                                {"children", ai::Json::array({"/home/dev/proj_a", "/home/dev/proj_b"})}});
                        } else {
                            cb({{"error", "unexpected path"}});
                        }
                    });

                    pane.ApplyWorkspaceSelection(false, false, localWsDir, L"/home/linux/workspace", "");
                    struct WsDlgForm { DLGTEMPLATE d{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,0,0,0,0,456,332}; WORD menu=0,cls=0,title=0; } wsDlgForm;
                    const HWND dlg1 = CreateDialogIndirectParamW(
                        GetModuleHandleW(nullptr), &wsDlgForm.d, GetAncestor(pane.window_, GA_ROOT),
                        AiPane::WorkspaceProc, reinterpret_cast<LPARAM>(&pane));
                    require(dlg1 != nullptr, "Failed to create WorkspaceProc dialog for Antigravity remote browse");
                    ShowWindow(dlg1, SW_SHOW);
                    UpdateWindow(dlg1);
                    pumpDlgAndAsync();

                    wchar_t dlgPath[512]{};
                    GetDlgItemTextW(dlg1, 213, dlgPath, 512);
                    const auto remoteItems = readComboItems(GetDlgItem(dlg1, 208));
                    require(fsCalls.size() == 2 &&
                                fsCalls[0] == std::pair<std::string, std::string>{"ai-settings", "/home/linux/workspace"} &&
                                fsCalls[1] == std::pair<std::string, std::string>{"ai-settings", "."} &&
                                std::wstring(dlgPath) == L"/home/dev" &&
                                remoteItems.size() == 3 &&
                                remoteItems[1] == L"proj_a" && remoteItems[2] == L"proj_b",
                            "Antigravity default remote SSH must fall back to '.' $HOME and list remote subdirectories");

                    SendMessageW(dlg1, WM_COMMAND, MAKEWPARAM(231, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dlg1, 231)));
                    const auto localItems = readComboItems(GetDlgItem(dlg1, 208));
                    require(pane.workspaceDialogIsLocal_ &&
                                std::find(localItems.begin(), localItems.end(), L"probe") != localItems.end() &&
                                std::find(localItems.begin(), localItems.end(), L"proj_a") == localItems.end(),
                            "Switching Remote -> Local must refresh Directory dropdown to local subdirectories");

                    pane.PopulateLocalDirectoryListing(dlg1, L"Z:\\serialctl_missing_invalid_dir_xyz");
                    require(SendDlgItemMessageW(dlg1, 208, CB_GETCOUNT, 0, 0) == 1,
                            "Invalid local directory must clear stale Directory dropdown entries");

                    pane.PopulateLocalDirectoryListing(dlg1, localWsDir);
                    require(SendDlgItemMessageW(dlg1, 208, CB_GETCOUNT, 0, 0) >= 2,
                            "Restoring valid local path must repopulate local Directory entries");

                    pane.settings_.host.clear();
                    SendMessageW(dlg1, WM_COMMAND, MAKEWPARAM(230, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dlg1, 230)));
                    pumpDlgAndAsync(200);
                    require(!pane.workspaceDialogIsLocal_ &&
                                SendDlgItemMessageW(dlg1, 208, CB_GETCOUNT, 0, 0) == 1,
                            "Switching Local -> Remote when remote browse is unavailable must clear local Directory entries");
                    DestroyWindow(dlg1);

                    // localMode == true with connected SSH session ("ssh-auto") + switching to Local cancels pending remote browse
                    pane.settings_.localMode = true;
                    pane.devices_ = [&](const std::string &op, const ai::Json &args) {
                        if (op == "remote_fs.sessions") {
                            return ai::Json::array({
                                {{"id", "ssh-auto"}, {"name", "dev"}, {"host", "10.0.0.9"}, {"directory", "/srv/remote-auto"}}});
                        }
                        return fsGuard.devices ? fsGuard.devices(op, args) : ai::Json(nullptr);
                    };
                    AiPane::RemoteFsCallback heldRemoteCb;
                    std::string autoSid = "unset";
                    pane.SetRemoteFsRunner([&](const std::string &op, const ai::Json &args, AiPane::RemoteFsCallback cb) {
                        if (op == "list") {
                            autoSid = args.value("sessionId", "");
                            heldRemoteCb = std::move(cb);
                        }
                    });
                    pane.ApplyWorkspaceSelection(false, false, localWsDir, L"/srv/remote-auto", "");
                    const HWND dlg2 = CreateDialogIndirectParamW(
                        GetModuleHandleW(nullptr), &wsDlgForm.d, GetAncestor(pane.window_, GA_ROOT),
                        AiPane::WorkspaceProc, reinterpret_cast<LPARAM>(&pane));
                    require(dlg2 != nullptr, "Failed to create WorkspaceProc dialog for localMode auto-SSH test");
                    ShowWindow(dlg2, SW_SHOW);
                    UpdateWindow(dlg2);
                    const auto captureUntil = GetTickCount64() + 1500;
                    while (!heldRemoteCb && GetTickCount64() < captureUntil) {
                        MSG m{};
                        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                            TranslateMessage(&m);
                            DispatchMessageW(&m);
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    require(heldRemoteCb && autoSid.empty() && pane.directoryPending_ &&
                                IsWindowEnabled(GetDlgItem(dlg2, 231)) != FALSE,
                            "localMode with connected SSH terminal must browse default session while leaving mode switch enabled");

                    SendMessageW(dlg2, WM_COMMAND, MAKEWPARAM(231, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dlg2, 231)));
                    const auto localAfterCancel = readComboItems(GetDlgItem(dlg2, 208));
                    GetDlgItemTextW(dlg2, 213, dlgPath, 512);
                    const std::wstring localPathAfterCancel = dlgPath;
                    std::error_code localPathError;
                    const bool sameLocalDirectory = std::filesystem::equivalent(
                        localPathAfterCancel, localWsDir, localPathError);
                    require(pane.workspaceDialogIsLocal_ && !pane.directoryPending_ &&
                                sameLocalDirectory && !localPathError &&
                                std::find(localAfterCancel.begin(), localAfterCancel.end(), L"probe") != localAfterCancel.end(),
                            "Clicking Local during pending remote browse must cancel pending wait and show local subdirectories");

                    heldRemoteCb({{"ok", true},
                                  {"sessionId", ""},
                                  {"path", "/srv/remote-auto"},
                                  {"children", ai::Json::array({"/srv/remote-auto/stale_dir"})}});
                    pumpDlgAndAsync(200);
                    GetDlgItemTextW(dlg2, 213, dlgPath, 512);
                    const auto afterStale = readComboItems(GetDlgItem(dlg2, 208));
                    require(pane.workspaceDialogIsLocal_ &&
                                std::wstring(dlgPath) == localPathAfterCancel &&
                                std::find(afterStale.begin(), afterStale.end(), L"stale_dir") == afterStale.end() &&
                                std::find(afterStale.begin(), afterStale.end(), L"probe") != afterStale.end(),
                            "Stale remote RemoteFsList response must be rejected after switching to Local");

                    // Switching back to Remote + selecting explicit SSH session in WsSshCombo passes sessionId == "ssh-auto"
                    heldRemoteCb = {};
                    SendMessageW(dlg2, WM_COMMAND, MAKEWPARAM(230, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dlg2, 230)));
                    require(!pane.workspaceDialogIsLocal_ && heldRemoteCb,
                            "Switching back to Remote must initiate remote browse");
                    heldRemoteCb({{"ok", true},
                                  {"sessionId", ""},
                                  {"path", "/srv/remote-auto"},
                                  {"children", ai::Json::array({"/srv/remote-auto/auto_sub"})}});
                    heldRemoteCb = {};
                    pumpDlgAndAsync(200);
                    require(readComboItems(GetDlgItem(dlg2, 208)).size() == 2,
                            "Default auto SSH browse must populate Directory items");
                    const HWND sshCombo = GetDlgItem(dlg2, 235);
                    SendMessageW(sshCombo, CB_SETCURSEL, 1, 0);
                    SendMessageW(dlg2, WM_COMMAND, MAKEWPARAM(235, CBN_SELCHANGE), reinterpret_cast<LPARAM>(sshCombo));
                    require(pane.workspaceDialogSshSessionId_ == "ssh-auto" && autoSid == "ssh-auto" &&
                                SendDlgItemMessageW(dlg2, 208, CB_GETCOUNT, 0, 0) == 1,
                            "Switching WsSshCombo session must clear previous Directory entries and query selected SSH session");
                    DestroyWindow(dlg2);
                }

                // Toggling AI localMode must never overwrite local/remote workspace choice
                pane.ApplyWorkspaceSelection(true, false, localWsDir, L"/srv/project-keep", "ssh-7");
                pane.settings_.localMode = true;
                require(pane.workspaceIsLocal_ && pane.localWorkspaceDirectory_ == localWsDir &&
                            pane.remoteWorkspaceDirectory_ == L"/srv/project-keep",
                        "AI localMode transition overwrote independent workspace state");

                // ConnectProc must pass Windows listening port into settings_.mcpPort (keeping remoteMcpPort == 0 for auto -R)
                {
                    const ai::ServerSettings diskOrig = ai::LoadServerSettings();
                    struct McpConnectGuard {
                        AiPane &pane;
                        std::function<uint16_t()> ensureOrig;
                        ai::ServerSettings diskOrig;
                        HWND dlg = nullptr;
                        ~McpConnectGuard() {
                            pane.client_.Shutdown();
                            if (dlg && IsWindow(dlg)) DestroyWindow(dlg);
                            pane.SetEnsureMcpPortProvider(std::move(ensureOrig));
                            std::wstring err;
                            ai::SaveServerSettings(diskOrig, err);
                        }
                    } mcpGuard{pane, pane.ensureMcpPort_, diskOrig};

                    struct DlgTpl { DLGTEMPLATE d{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 0, 0, 0, 0, 270, 260}; WORD menu = 0, cls = 0, title = 0; } connForm;
                    pane.settings_.provider = ai::AiProvider::Antigravity;
                    pane.settings_.localMode = false;
                    pane.settings_.host = L"serialctl-offline-mcp-" + std::to_wstring(GetCurrentProcessId()) + L".invalid";
                    pane.settings_.port = 22;
                    pane.settings_.platformMode = false;
                    pane.settings_.username = L"fixture";
                    pane.settings_.password = L"fixture-only-π";
                    pane.settings_.rememberPassword = false;
                    pane.settings_.mcpPort = 7080;
                    pane.settings_.remoteMcpPort = 9999;

                    pane.SetEnsureMcpPortProvider([]() -> uint16_t { return 43219; });
                    mcpGuard.dlg = CreateDialogIndirectParamW(
                        GetModuleHandleW(nullptr), &connForm.d, GetAncestor(pane.window_, GA_ROOT),
                        AiPane::ConnectProc, reinterpret_cast<LPARAM>(&pane));
                    require(mcpGuard.dlg != nullptr, "Failed to create ConnectProc dialog for MCP port test");
                    CheckDlgButton(mcpGuard.dlg, 204 /*Remember*/, BST_UNCHECKED);
                    CheckDlgButton(mcpGuard.dlg, 214 /*PortMode*/, BST_UNCHECKED);
                    SendDlgItemMessageW(mcpGuard.dlg, 216 /*ProviderCombo*/, CB_SETCURSEL, 1, 0);
                    SendDlgItemMessageW(mcpGuard.dlg, 217 /*ConnModeCombo*/, CB_SETCURSEL, 0, 0);
                    SendMessageW(mcpGuard.dlg, WM_COMMAND, MAKEWPARAM(206 /*Test*/, BN_CLICKED),
                                 reinterpret_cast<LPARAM>(GetDlgItem(mcpGuard.dlg, 206)));
                    require(pane.settings_.provider == ai::AiProvider::Antigravity && !pane.settings_.localMode &&
                                pane.settings_.mcpPort == 43219 && pane.settings_.remoteMcpPort == 0,
                            "Remote Antigravity ConnectProc must set mcpPort from ensureMcpPort_ and keep remoteMcpPort == 0");
                    pane.client_.Shutdown();

                    pane.SetEnsureMcpPortProvider([]() -> uint16_t { return 43220; });
                    pane.settings_.mcpPort = 7080;
                    SendDlgItemMessageW(mcpGuard.dlg, 217 /*ConnModeCombo*/, CB_SETCURSEL, 1, 0);
                    SendMessageW(mcpGuard.dlg, WM_COMMAND, MAKEWPARAM(206 /*Test*/, BN_CLICKED),
                                 reinterpret_cast<LPARAM>(GetDlgItem(mcpGuard.dlg, 206)));
                    require(pane.settings_.provider == ai::AiProvider::Antigravity && pane.settings_.localMode &&
                                pane.settings_.mcpPort == 43220 && pane.settings_.remoteMcpPort == 0,
                            "Local Antigravity ConnectProc must also set mcpPort from ensureMcpPort_ and keep remoteMcpPort == 0");
                    pane.client_.Shutdown();

                    HKEY mcpHostKey = nullptr;
                    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SerialCtl\\TrustedHostKeys", 0, KEY_SET_VALUE, &mcpHostKey) == ERROR_SUCCESS) {
                        RegDeleteValueW(mcpHostKey, (pane.settings_.host + L":22").c_str());
                        RegCloseKey(mcpHostKey);
                    }
                }
            } catch (...) {
                pane.busy_ = false;
                pane.taskAuthorized_ = false;
                pane.client_.Shutdown();
                pane.settings_ = origSettings;
                pane.ApplyWorkspaceSelection(origWs.isLocal, origWs.localReadOnly, origWs.localDirectory,
                                             origWs.remoteDirectory, origWs.remoteSshSessionId);
                std::error_code remEc;
                std::filesystem::remove_all(localWsDir, remEc);
                throw;
            }
            pane.busy_ = false;
            pane.taskAuthorized_ = false;
            pane.settings_ = origSettings;
            pane.ApplyWorkspaceSelection(origWs.isLocal, origWs.localReadOnly, origWs.localDirectory,
                                         origWs.remoteDirectory, origWs.remoteSshSessionId);
            std::filesystem::remove_all(localWsDir, ec);
        }

        // Verify thinking/tool formatting, interleaved turn order, and Antigravity resume/cache/workspace restoration
        {
            const auto savedSettings = pane.settings_;
            const bool savedLocal = pane.workspaceIsLocal_;
            const bool savedReadOnly = pane.localWorkspaceReadOnly_;
            const auto savedLocalDir = pane.localWorkspaceDirectory_;
            const auto savedRemoteDir = pane.remoteWorkspaceDirectory_;
            const auto savedSshSid = pane.remoteSshSessionId_;
            std::error_code ec;
            const auto resumeLocalDir = std::filesystem::temp_directory_path() /
                (L"serialctl-pane-resume-ws-" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(resumeLocalDir, ec);

            struct ResumeTestGuard {
                AiPane &pane;
                ai::ServerSettings settings;
                bool isLocal, readOnly;
                std::wstring localDir, remoteDir;
                std::string sshSid;
                std::filesystem::path tempDir;
                ~ResumeTestGuard() {
                    pane.busy_ = false;
                    pane.pendingHistoryRestore_ = false;
                    pane.pendingHistoryBinding_ = {};
                    pane.isProbeSession_ = false;
                    pane.thread_.clear();
                    pane.turn_.clear();
                    pane.displayGroup_.clear();
                    pane.messages_.clear();
                    pane.requests_.clear();
                    pane.transcriptCache_.clear();
                    pane.transcriptCacheOrder_.clear();
                    pane.groupStates_.clear();
                    pane.groupDurations_.clear();
                    pane.collapsedGroups_.clear();
                    pane.initializedGroups_.clear();
                    pane.userExpandedGroups_.clear();
                    pane.prevTurnDiffs_.clear();
                    pane.currTurnDiffs_.clear();
                    pane.settings_ = settings;
                    pane.ApplyWorkspaceSelection(isLocal, readOnly, localDir, remoteDir, sshSid);
                    pane.Reflow();
                    std::error_code remEc;
                    std::filesystem::remove_all(tempDir, remEc);
                }
            } resumeGuard{pane, savedSettings, savedLocal, savedReadOnly, savedLocalDir, savedRemoteDir, savedSshSid, resumeLocalDir};

            pane.busy_ = false;
            pane.stopRequested_ = false;
            pane.deferReflow_ = false;
            pane.displayGroup_.clear();
            pane.ClearTextSelection();
            pane.messages_.clear();
            pane.selectedMessage_ = SIZE_MAX;
            pane.messageBytes_ = 0;
            pane.collapsedGroups_.clear();
            pane.initializedGroups_.clear();
            pane.userExpandedGroups_.clear();
            pane.prevTurnDiffs_.clear();
            pane.currTurnDiffs_.clear();
            pane.groupStates_.clear();
            pane.groupDurations_.clear();
            pane.transcriptCache_.clear();
            pane.transcriptCacheOrder_.clear();

            // 1. Thinking uses full proseMarkup; Tool uses inlineFormatted without breaking pipes or underscores
            pane.Add(L"思考", L"### 分析步骤\n\n- 检查 `list`\n- 对照输出", "think-fmt");
            pane.Add(L"工具",
                     L"**Run mcp_serialctl_workspace.list** · 已完成\n\n```json\n{\"depth\": 2}\n```\n\n**输出**\n```text\nsrc/\n```\n| col1 | col2 |\n| --- | --- |",
                     "server:fmt-1", L"{\"rawOutput\":{\"entries\":[\"src/\"]}}");
            pane.Reflow();
            require(pane.messages_.size() == 2 &&
                        pane.messages_[0].paragraphs.size() >= 3 &&
                        pane.messages_[0].paragraphs[0].headingLevel == 3 &&
                        pane.messages_[0].paragraphs[0].text == L"分析步骤" &&
                        !pane.messages_[0].paragraphs[1].spans.empty() &&
                        pane.messages_[0].paragraphs[1].text.find(L"list") != std::wstring::npos,
                    "Thinking bubble must lay out headings, inline code spans and list items");
            require(pane.messages_[1].blocks.size() == 2 &&
                        pane.messages_[1].blocks[0].kind == ai::markup::Kind::Code &&
                        pane.messages_[1].blocks[1].kind == ai::markup::Kind::Code &&
                        !pane.messages_[1].paragraphs.empty() &&
                        pane.messages_[1].paragraphs[0].spans.size() == 1 &&
                        pane.messages_[1].paragraphs[0].spans[0].ink == ai::markup::Ink::Bold &&
                        pane.messages_[1].paragraphs[0].text == L"Run mcp_serialctl_workspace.list · 已完成" &&
                        pane.messages_[1].paragraphs.back().text == L"| --- | --- |",
                    "Tool bubble must apply bold/code inline formatting while keeping literal pipes as prose");
            {
                HDC screenDc = GetDC(nullptr);
                HDC memDc = CreateCompatibleDC(screenDc);
                HBITMAP bmp = CreateCompatibleBitmap(screenDc, 820, 520);
                HGDIOBJ oldBmp = SelectObject(memDc, bmp);
                HGDIOBJ oldFont = SelectObject(memDc, pane.font_);
                pane.PaintTranscript(memDc);
                SelectObject(memDc, oldFont);
                SelectObject(memDc, oldBmp);
                DeleteObject(bmp);
                DeleteDC(memDc);
                ReleaseDC(nullptr, screenDc);
            }

            // 2. Interleaved live ACP turn events: thought -> commentary -> tool -> thought2 -> final
            pane.settings_.provider = ai::AiProvider::Antigravity;
            pane.settings_.localMode = false;
            pane.messages_.clear();
            pane.thread_ = "acp-thread-a";
            pane.isProbeSession_ = false;
            pane.turn_ = "acp-turn-1";
            pane.busy_ = true;
            pane.showTurnProgress_ = true;
            pane.displayGroup_ = "acp-turn-1";
            pane.groupStates_["acp-turn-1"] = L"进行中";
            pane.turnStarted_ = GetTickCount64();

            pane.Event({{"event", "thinkingDelta"}, {"conversationId", "acp-thread-a"}, {"taskId", "acp-turn-1"},
                        {"messageId", "thinking:acp-turn-1"}, {"text", u8"先思考目录结构"}});
            pane.Event({{"event", "textDelta"}, {"conversationId", "acp-thread-a"}, {"taskId", "acp-turn-1"},
                        {"messageId", "acp-msg-acp-turn-1"}, {"text", u8"先列出根目录。"}});
            pane.Event({{"event", "textComplete"}, {"conversationId", "acp-thread-a"}, {"taskId", "acp-turn-1"},
                        {"messageId", "acp-msg-acp-turn-1"}, {"text", u8"先列出根目录。"}, {"final", false}});
            pane.Event({{"event", "serverTool"}, {"conversationId", "acp-thread-a"}, {"taskId", "acp-turn-1"},
                        {"messageId", "server:t1"}, {"completed", true}, {"failed", false}, {"appendStream", false},
                        {"technical", "{\"rawOutput\":{\"entries\":[\"src/\"]}}"},
                        {"text", u8"**Run mcp_workspace_list** · 已完成\n\n**输出**\n```text\nsrc/\n```"}});
            pane.Event({{"event", "thinkingDelta"}, {"conversationId", "acp-thread-a"}, {"taskId", "acp-turn-1"},
                        {"messageId", "thinking:acp-turn-1:2"}, {"text", u8"工具返回 src/"}});
            pane.Event({{"event", "textDelta"}, {"conversationId", "acp-thread-a"}, {"taskId", "acp-turn-1"},
                        {"messageId", "acp-msg-acp-turn-1-2"}, {"text", u8"总结：发现 src 目录。"}});
            pane.Event({{"event", "textComplete"}, {"conversationId", "acp-thread-a"}, {"taskId", "acp-turn-1"},
                        {"messageId", "acp-msg-acp-turn-1-2"}, {"text", u8"总结：发现 src 目录。"}, {"final", true}});
            pane.Event({{"event", "taskFinished"}, {"conversationId", "acp-thread-a"}, {"taskId", "acp-turn-1"},
                        {"status", "completed"}, {"message", ""}});

            require(pane.messages_.size() == 5 &&
                        pane.messages_[0].role == L"思考" && pane.messages_[0].item == "thinking:acp-turn-1" &&
                        pane.messages_[1].role == L"AI" && pane.messages_[1].process &&
                        pane.messages_[2].role == L"工具" && pane.messages_[2].technical.find(L"rawOutput") != std::wstring::npos &&
                        pane.messages_[3].role == L"思考" && pane.messages_[3].item == "thinking:acp-turn-1:2" &&
                        pane.messages_[4].role == L"AI" && !pane.messages_[4].process &&
                        pane.messages_[0].groupHeader && pane.messages_[0].folded &&
                        pane.messages_[1].hidden && pane.messages_[2].hidden && pane.messages_[3].hidden &&
                        !pane.messages_[4].hidden && !pane.messages_[4].folded,
                    "Interleaved thinking, commentary and tool process bubbles must stay chronological and fold on completion");

            // 3a. Switching from thread-a to thread-b when Antigravity server returns no messages must clear thread-a
            pane.requests_[801] = {"resume", "", pane.workspaceDirectory_, 0};
            pane.Event({{"event", "response"},
                        {"token", 801},
                        {"result", {{"conversationId", "acp-thread-b"}, {"messages", ai::Json::array()}}}});
            require(pane.thread_ == "acp-thread-b" &&
                        pane.messages_.size() == 1 && pane.messages_[0].role == L"状态" &&
                        pane.messages_[0].text.find(L"acp-thread-b") != std::wstring::npos,
                    "Switching to Antigravity session without replay must clear stale transcript instead of showing old thread");

            // 3b. Reconnect on same thread-b (non-probe) must preserve current transcript rather than duplicating status
            pane.Add(L"我", L"继续查日志", "local-u1");
            pane.requests_[802] = {"resume", "", pane.workspaceDirectory_, 0};
            pane.Event({{"event", "response"},
                        {"token", 802},
                        {"result", {{"conversationId", "acp-thread-b"}, {"messages", ai::Json::array()}}}});
            require(pane.thread_ == "acp-thread-b" && pane.messages_.size() == 2 &&
                        pane.messages_[1].text == L"继续查日志",
                    "Same-thread Antigravity resume without server replay must retain in-memory transcript");

            // 3c. Switching back to thread-a with empty server replay restores from local transcriptCache_
            pane.requests_[803] = {"resume", "", pane.workspaceDirectory_, 0};
            pane.Event({{"event", "response"},
                        {"token", 803},
                        {"result", {{"conversationId", "acp-thread-a"}, {"messages", ai::Json::array()}}}});
            require(pane.thread_ == "acp-thread-a" && pane.messages_.size() == 5 &&
                        pane.messages_[2].technical.find(L"rawOutput") != std::wstring::npos &&
                        pane.messages_[4].text == L"总结：发现 src 目录。",
                    "Returning to cached Antigravity session must restore transcript and technical details");

            // 3d. Resuming with server-replayed messages + pendingHistoryBinding_ localReadOnly
            pane.pendingHistoryRestore_ = true;
            pane.pendingHistoryTarget_.clear();
            pane.pendingHistoryBinding_ = {true, true, true, resumeLocalDir.u8string(), "/home/linux/old", "ssh-9"};
            pane.requests_[804] = {"resume", "", pane.workspaceDirectory_, 0};
            pane.Event({{"event", "response"},
                        {"token", 804},
                        {"result",
                         {{"conversationId", "acp-thread-c"},
                          {"messages",
                           ai::Json::array({{{"role", u8"我"}, {"text", u8"读只读目录"}, {"messageId", "u1"}, {"group", "g1"}, {"status", "completed"}},
                                            {{"role", u8"工具"}, {"text", u8"**Run mcp_workspace_list** · 已完成"}, {"technical", "{\"entries\":[]}"},
                                             {"messageId", "server:t9"}, {"process", true}, {"group", "g1"}, {"status", "completed"}, {"failedStep", false}},
                                            {{"role", "AI"}, {"text", u8"完成"}, {"messageId", "a1"}, {"group", "g1"}, {"status", "completed"}}})}}}});
            require(pane.thread_ == "acp-thread-c" && pane.workspaceIsLocal_ && pane.localWorkspaceReadOnly_ &&
                        pane.workspaceVerified_ && !pane.pendingHistoryRestore_ &&
                        pane.messages_.size() == 3 &&
                        pane.messages_[1].technical == L"{\"entries\":[]}" &&
                        pane.messages_[1].folded && !pane.messages_[2].folded,
                    "History resume must restore localReadOnly workspace binding, technical metadata and folded process state");

            // 3e. Scheme A SidebarCallbacks + compact exec / file change cards + deep-link clicks
            std::vector<ai::ExecProgressEvent> cbExecs;
            std::vector<std::string> cbFocusExecs;
            std::vector<ai::FileChangeRecord> cbLastChanges;
            std::uint64_t cbLastTurnSeq = 0;
            std::string cbLastFocusChange;
            {
                AiPane::SidebarCallbacks sc;
                sc.onExecEvent = [&](const ai::ExecProgressEvent &ev) { cbExecs.push_back(ev); };
                sc.onFocusExec = [&](const std::string &id) { cbFocusExecs.push_back(id); };
                sc.onFileChangesUpdated = [&](const std::vector<ai::FileChangeRecord> &changes,
                                              std::uint64_t turnSeq, const std::string &focusId, bool) {
                    cbLastChanges = changes;
                    cbLastTurnSeq = turnSeq;
                    cbLastFocusChange = focusId;
                };
                pane.SetSidebarCallbacks(std::move(sc));
            }
            const bool savedBusy = pane.busy_;
            const bool savedStop = pane.stopRequested_;
            const auto savedTurn = pane.turn_;
            pane.busy_ = true;
            pane.stopRequested_ = false;
            pane.turn_ = "turn-3e";
            pane.Event({{"event", "serverTool"},
                        {"messageId", "srv:exec1"},
                        {"taskId", "turn-3e"},
                        {"execKind", "command"},
                        {"execId", "srv-exec-1"},
                        {"command", "cmake --build build"},
                        {"cwd", "/home/proj"},
                        {"completed", false}});
            pane.Event({{"event", "serverToolDelta"},
                        {"messageId", "srv:exec1"},
                        {"taskId", "turn-3e"},
                        {"execDelta", true},
                        {"text", "[1/2] Compiling...\n"}});
            pane.Event({{"event", "serverTool"},
                        {"messageId", "srv:exec1"},
                        {"taskId", "turn-3e"},
                        {"execKind", "command"},
                        {"execId", "srv-exec-1"},
                        {"command", "cmake --build build"},
                        {"exitCode", 0},
                        {"failed", false},
                        {"completed", true}});
            pane.Event({{"event", "serverTool"},
                        {"messageId", "srv:chg1"},
                        {"taskId", "turn-3e"},
                        {"fileChanges", ai::Json::array({{{"changeId", "srv-chg-1"},
                                                          {"action", "edit"},
                                                          {"path", "src/app/Foo.cpp"},
                                                          {"addedLines", 5},
                                                          {"removedLines", 2},
                                                          {"diff", "--- a/src/app/Foo.cpp\n+++ b/src/app/Foo.cpp\n@@ -1 +1 @@\n-old\n+new\n"}}})},
                        {"completed", true}});
            pane.busy_ = savedBusy;
            pane.stopRequested_ = savedStop;
            pane.turn_ = savedTurn;
            require(cbExecs.size() == 3 &&
                        cbExecs[0].phase == ai::ExecProgressEvent::Phase::Started &&
                        cbExecs[1].phase == ai::ExecProgressEvent::Phase::OutputDelta &&
                        cbExecs[2].phase == ai::ExecProgressEvent::Phase::Completed &&
                        cbExecs[2].exitCode == 0,
                    "SidebarCallbacks must receive live exec Started/Delta/Completed events");
            require(!pane.FileChanges().empty() &&
                        pane.FileChanges().back().changeId == "srv-chg-1" &&
                        pane.FileChanges().back().path == "src/app/Foo.cpp",
                    "File change toolUse must record change in AiPane::FileChanges()");
            pane.ActivateLink(L"serialctl-exec://srv-exec-1");
            pane.ActivateLink(L"serialctl-change://srv-chg-1");
            require(!cbFocusExecs.empty() && cbFocusExecs.back() == "srv-exec-1" &&
                        cbLastFocusChange == "srv-chg-1",
                    "ActivateLink must dispatch serialctl-exec:// and serialctl-change:// deep links");

            // 3f. RemoteSnapshotAfter records FileChanges without appending a file-list bubble to transcript
            pane.NewConversation();
            pane.thread_ = "snap-thread";
            pane.ApplyWorkspaceSelection(false, false, {}, L"/home/proj", "ssh-1");
            pane.turnSeq_ = 1;
            const size_t msgCountBeforeSnap = pane.messages_.size();
            {
                AiPane::AsyncWorkResult snap;
                snap.kind = AiPane::AsyncWorkResult::Kind::RemoteSnapshotAfter;
                snap.conversationId = "snap-thread";
                snap.workspace = L"/home/proj";
                snap.seq = 1;
                snap.args = {{"complete", true}, {"root", ai::Json::array({"/home/proj", 1, 2})},
                             {"files", {{"a.cpp", {{"digest", "h1"}, {"text", "int x = 1;\n"}}}}}};
                snap.result = {{"complete", true}, {"root", ai::Json::array({"/home/proj", 1, 2})},
                               {"files", {{"a.cpp", {{"digest", "h2"}, {"text", "int x = 2;\n"}}}}}};
                pane.EnqueueAsyncWork(std::move(snap));
                pane.ProcessAsyncWork();
            }
            require(pane.messages_.size() == msgCountBeforeSnap &&
                        pane.FileChanges().size() == 1 && pane.FileChanges()[0].path == "a.cpp",
                    "RemoteSnapshotAfter must record changes in sidebar without appending a reply-tail card");

            // 3g. Cumulative turnDiff across turns must filter out unchanged prior-turn files
            pane.prevTurnDiffs_.clear();
            pane.currTurnDiffs_.clear();
            pane.fileChanges_.clear();
            pane.turnSeq_ = 1;
            pane.busy_ = true;
            pane.stopRequested_ = false;
            pane.turn_ = "td-1";
            const std::string diffA1 = "--- a/a.cpp\n+++ b/a.cpp\n@@ -1 +1 @@\n-1\n+2\n";
            const std::string diffB1 = "--- a/b.cpp\n+++ b/b.cpp\n@@ -1 +1 @@\n-3\n+4\n";
            const std::string diffA2 = "--- a/a.cpp\n+++ b/a.cpp\n@@ -1 +1 @@\n-1\n+5\n";
            pane.Event({{"event", "turnDiff"}, {"conversationId", "snap-thread"}, {"taskId", "td-1"},
                        {"fileChanges", ai::Json::array({{{"path", "a.cpp"}, {"action", "edit"}, {"diff", diffA1},
                                                          {"detail", ""}, {"addedLines", 1}, {"removedLines", 1}, {"truncated", false}}})}});
            pane.Event({{"event", "taskFinished"}, {"conversationId", "snap-thread"}, {"taskId", "td-1"}, {"status", "completed"}});
            // Simulate turn 2 starting with SendPrompt's baseline advance
            for (const auto &[p, fp] : pane.currTurnDiffs_) pane.prevTurnDiffs_[p] = fp;
            pane.currTurnDiffs_.clear();
            pane.turnSeq_ = 2;
            pane.busy_ = true;
            pane.turn_ = "td-2";
            pane.Event({{"event", "turnDiff"}, {"conversationId", "snap-thread"}, {"taskId", "td-2"},
                        {"fileChanges", ai::Json::array({
                            {{"path", "a.cpp"}, {"action", "edit"}, {"diff", diffA1}, {"detail", ""}, {"addedLines", 1}, {"removedLines", 1}, {"truncated", false}},
                            {{"path", "b.cpp"}, {"action", "edit"}, {"diff", diffB1}, {"detail", ""}, {"addedLines", 1}, {"removedLines", 1}, {"truncated", false}}})}});
            pane.Event({{"event", "taskFinished"}, {"conversationId", "snap-thread"}, {"taskId", "td-2"}, {"status", "completed"}});
            size_t turn2Count = 0;
            std::string turn2OnlyPath;
            for (const auto &fc : pane.FileChanges()) {
                if (fc.turnSeq == 2) { ++turn2Count; turn2OnlyPath = fc.path; }
            }
            require(turn2Count == 1 && turn2OnlyPath == "b.cpp",
                    "Cumulative turnDiff must exclude unchanged turn-1 files from turn-2 changes");
            for (const auto &[p, fp] : pane.currTurnDiffs_) pane.prevTurnDiffs_[p] = fp;
            pane.currTurnDiffs_.clear();
            pane.turnSeq_ = 3;
            pane.busy_ = true;
            pane.turn_ = "td-3";
            pane.Event({{"event", "turnDiff"}, {"conversationId", "snap-thread"}, {"taskId", "td-3"},
                        {"fileChanges", ai::Json::array({
                            {{"path", "a.cpp"}, {"action", "edit"}, {"diff", diffA2}, {"detail", ""}, {"addedLines", 1}, {"removedLines", 1}, {"truncated", false}},
                            {{"path", "b.cpp"}, {"action", "edit"}, {"diff", diffB1}, {"detail", ""}, {"addedLines", 1}, {"removedLines", 1}, {"truncated", false}}})}});
            pane.Event({{"event", "taskFinished"}, {"conversationId", "snap-thread"}, {"taskId", "td-3"}, {"status", "completed"}});
            size_t turn3Count = 0;
            std::string turn3OnlyPath;
            for (const auto &fc : pane.FileChanges()) {
                if (fc.turnSeq == 3) { ++turn3Count; turn3OnlyPath = fc.path; }
            }
            require(turn3Count == 1 && turn3OnlyPath == "a.cpp",
                    "Modifying a previously edited file again with a new diff must record under the new turn");
            pane.busy_ = false;
            pane.SetSidebarCallbacks({});
        }

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
int main(int argc, char **argv) {
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
            const bool layoutAudit=argc>1 && std::string(argv[1])=="--layout-audit";
            const bool composer=argc>1 && std::string(argv[1])=="--composer";
            const bool replyFormat=argc>1 && std::string(argv[1])=="--reply-format";
            const bool voiceComposer=argc>1 && std::string(argv[1])=="--voice-composer";
            if(!layoutAudit && !composer && !replyFormat && !voiceComposer) CheckHostKeyDialogs(parent, font);
            if (!pane.Create(parent, font, false, [](const std::string &method, const ai::Json &) {
                    if (method == "sessions")
                        return ai::Json::array({{{"id", "session-7"}, {"name", "192.0.2.20 / COM3"}}});
                    return ai::Json{{"error", "Offline UI test does not open or write devices"}};
                }))
                throw std::runtime_error("Cannot create native AI pane");
            SetWindowPos(pane.Handle(), nullptr, 0, 0, 1000, 650, SWP_NOZORDER | SWP_SHOWWINDOW);
            if(composer) AiPaneTestAccess::RunComposer(pane);
            else if(layoutAudit) AiPaneTestAccess::AuditLayoutLoad(pane);
            else if(replyFormat) AiPaneTestAccess::RunReplyFormat(pane);
            else if(voiceComposer) AiPaneTestAccess::RunVoiceComposer(pane);
            else AiPaneTestAccess::Run(pane);
            if (!composer && !replyFormat && !voiceComposer) std::cout << "AiPane: explicit remote selection, wheel/key/scrollbar history navigation, streaming scroll "
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
