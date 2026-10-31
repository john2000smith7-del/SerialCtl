#include "TerminalModel.h"
#include "CmdLineEditor.h"
#include "BootInterrupt.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

void ExpectLine(const serialctl::TerminalModel& model, size_t line,
    const std::wstring& expected, const char* message) {
    if (model.LineText(line) == expected) return;
    std::wcerr << L"FAILED: " << message << L" expected=[" << expected <<
        L"] actual=[" << model.LineText(line) << L"]\n";
    ++failures;
}

void TestSharedResponseOwnership() {
    using Model = serialctl::TerminalModel;
    const std::wstring query = L"\x1b[5n\x1b[6n\x1b[c\x1b[>c\x1b[18t\x1b[19t";
    const std::string expected = "\x1b[0n\x1b[1;1R\x1b[?1;2c\x1b[>0;136;0c\x1b[8;24;80t\x1b[8;24;80t";
    Model owner(80, 24), first(100, 30), second(120, 40);
    first.SetResponseRole(Model::ResponseRole::Viewer);
    second.SetResponseRole(Model::ResponseRole::Viewer);
    Expect(owner.Feed(query, L"").response == expected, "owner alone answers DSR, DA and size queries");
    Expect(first.Feed(query, L"").response.empty() && second.Feed(query, L"").response.empty(),
           "two differently sized live viewers produce no automatic input");
    first.Resize(132, 50);
    first.Clear();
    first.ClearScrollback();
    first.Reset();
    // Includes remote RIS and alternate-screen changes: remote output cannot
    // grant ownership to a viewing model, even inside one Feed call.
    const auto replay = first.Feed(std::wstring(L"\x1b") + L"c\x1b[?1049h" + query +
                                      L"\x1b[?1049lhello", L"");
    Expect(replay.response.empty(), "resized/reset viewer and history RIS never regain response ownership");
    ExpectLine(first, 0, L"hello", "viewer still renders after reset and alternate-screen replay");
    Expect(replay.logText == L"hello", "query suppression preserves ordinary log output");
    for (wchar_t c : query)
        Expect(second.Feed(std::wstring(1, c), L"").response.empty(), "split query never creates viewer response");
    Model ssh(90, 28), telnet(70, 20);
    Expect(ssh.Feed(query, L"").response.find("\x1b[8;28;90t") != std::string::npos,
           "independent SSH model retains its own terminal geometry replies");
    telnet.Feed(L"\x1b[?1049h\x1b[3;4H", L"");
    Expect(telnet.Feed(L"\x1b[6n", L"").response == "\x1b[3;4R",
           "independent Telnet alternate screen retains cursor replies");
}

void TestEnhancedTuiInputAndExit() {
    serialctl::TerminalModel model(40,8);
    model.Feed(L"shell\x1b[3;11H\x1b[31m\x1b" L"7", L"");
    const auto before = model.Feed(L"\x1b[6n",L"").response;
    Expect(model.Feed(L"\x1b[?u",L"").response == "\x1b[?0u", "Kitty query receives actual supported mode");
    Expect(model.Feed(L"\x1b[6n",L"").response == before, "Prefixed u query never restores the cursor");
    model.Feed(L"\x1b[>31u",L"");
    Expect(model.KeyboardFlags() == 1 && model.KeySequence("Enter") == "\x1b[13u", "Only implemented disambiguation mode negotiated; enhanced Enter is a key");
    Expect(model.KeySequence("c",4) == "\x1b[99;5u", "Enhanced Ctrl+C keeps key/modifier identity");
    model.Feed(L"\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[?2004h\x1b[>1u\x1b[2;2H\x1b[32m\x1b" L"7",L"");
    Expect(model.WheelSequence(true,4,2,1) == "\x1b[<68;5;3M", "Shift wheel reaches the application using SGR mouse coordinates");
    Expect(model.MouseSequence(0,4,2,0,true) == "\x1b[<0;5;3m", "SGR mouse release is distinct from press");
    Expect(model.Feed(L"\x1b[?1006$p",L"").response == "\x1b[?1006;1$y", "Mouse mode query reports actual state");
    model.Resize(52,10);
    model.Feed(L"\x1b[?1049l",L"");
    Expect(!model.AlternateScreen() && !model.BracketedPaste() && !model.MouseCaptured() && model.KeyboardFlags()==1,
        "TUI exit restores primary paste/mouse/keyboard modes after resize");
    model.Feed(L"\x1b[<u",L"");
    Expect(model.KeyboardFlags()==0 && model.KeySequence("Enter")=="\r" && model.KeySequence("c",4)==std::string(1,3),
        "Keyboard mode pop restores real legacy CR and Ctrl+C bytes");
    model.Feed(L"\x1b" L"8X",L"");
    Expect(model.CurrentLineText().find(L'X')!=std::wstring::npos, "Shell renders after leaving a resized TUI");
    serialctl::TerminalModel viewer(40,8); viewer.SetResponseRole(serialctl::TerminalModel::ResponseRole::Viewer);
    Expect(viewer.Feed(L"\x1b[?u\x1b[?1006$p",L"").response.empty(),"Mirrored viewers never answer input-mode queries");
}
void TestLocalBootInterrupt() {
    serialctl::BootInterrupt boot;
    Expect(!boot.Arm("", " ", "=>",90,0), "Empty U-Boot matcher rejected");
    Expect(boot.Arm("Hit any key", " ", "=>",90,100), "Bounded actual-board matcher armed");
    Expect(boot.Feed("Hit any ",8,101).empty(), "Split prompt waits for remaining receive bytes");
    Expect(boot.Feed("key to stop",11,102)==" ", "Receive path interrupts immediately across packet boundaries");
    Expect(boot.Poll(181).empty() && boot.Poll(182)==" ", "Retry uses a local 80ms interval");
    Expect(boot.Poll(262)==" " && boot.Poll(1000).empty(), "At most three interrupt bursts");
    Expect(!boot.Status(1000).entered,"Submission is never reported as bootloader entry");
    boot.Feed("\r\n=> ",5,1001);
    Expect(boot.Status(1001).entered && boot.Poll(2000).empty(),"Only fresh boot prompt confirms entry and stops retries");
    boot.Arm("Hit", "stop", "=>",1,2000); boot.Cancel();
    Expect(boot.Feed("Hit",3,2001).empty() && boot.Status(2001).state=="canceled","Cancel cannot submit later interrupt");
    boot.Arm("Hit", " ", "=>",1,3000); boot.Poll(4000);
    Expect(boot.Status(4000).state=="timed_out" && !boot.Status(4000).entered,"Timeout is explicit failure to verify entry");
    boot.Arm("Hit", " ", "=>",1,5000); boot.Delivery(false,"Disconnected");
    Expect(boot.Status(5001).state=="failed" && boot.Poll(5002).empty(),"Transport failure cancels pending retries");
    // After the bounded tail drops its front, offset 0 is not necessarily a line start.
    boot.Arm("Hit", " ", "=>",90,6000);
    boot.Feed(std::string(500,'x').c_str(),500,6001);
    const std::string promptLookalike="=> "+std::string(509,'q');
    boot.Feed(promptLookalike.c_str(),promptLookalike.size(),6002);
    boot.Feed(std::string(512,'q').c_str(),512,6003);
    boot.Feed("q",1,6004);
    Expect(!boot.Status(6004).entered,"A prompt lookalike exposed only by tail trimming is not a line-start prompt");
    boot.Feed("\r\n=> ",5,6005);
    Expect(boot.Status(6005).entered,"A genuine line-start prompt is still recognized after trimming");
    Expect(boot.PreparePreset("uboot_space",0,7000),"U-Boot preset prepares without a keyword or automatic deadline");
    Expect(boot.Poll(7000).empty(),"No key can precede an accepted reboot");
    boot.RestartSubmitted(7001);
    Expect(boot.Poll(7001)==" ","First space is immediate after reboot submission");
    Expect(boot.Poll(7300).empty(),"Default cadence cannot submit faster than 300ms");
    for(unsigned i=1;i<=20;++i) Expect(boot.Poll(7001+i*300)==" ","Continuous preset is not limited to three bursts");
    Expect(!boot.PreparePreset("phytium_uefi_f2",0,13002),"Another active preset cannot interleave a second reboot");
    boot.Feed("\r\n=> ",5,13003);
    Expect(boot.Status(13003).entered && boot.Poll(13100).empty(),"Actual U-Boot prompt stops continuous spaces");
    Expect(boot.PreparePreset("phytium_uefi_f2",0,14000,500),"Phytium UEFI preset supports 500ms cadence");
    boot.RestartSubmitted(14000);
    Expect(boot.Poll(14000)=="\x1bOQ","F2 uses the VT100 key sequence rather than literal F2");
    const std::string banner="Phytium UEFI firmware: Press F2 to enter setup\r\n";
    boot.Feed(banner.data(),banner.size(),14001);
    Expect(!boot.Status(14001).entered && boot.Poll(14499).empty() && boot.Poll(14500)=="\x1bOQ","UEFI banner is not menu entry; requested 500ms interval is respected");
    const std::string menu="\r\nBoot Manager\r\nBoot Maintenance Manager\r\n";
    boot.Feed(menu.data(),menu.size(),14501);
    Expect(boot.Status(14501).entered && boot.Poll(15000).empty(),"Actual UEFI menu stops repeated F2");
    boot.PreparePreset("uboot_space",1,16000); boot.RestartSubmitted(16000);
    Expect(boot.Poll(17000).empty() && boot.Status(17000).state=="timed_out","Explicit timeout is still supported");
    boot.PreparePreset("uboot_space",0,18000); boot.RestartSubmitted(18000);boot.Poll(18000);
    Expect(boot.Poll(18000+24*60*60*1000ull)==" " && boot.Status(18000+24*60*60*1000ull).untilStopped,"Default continuous mode has no hidden count or 90/120-second cutoff");
    boot.Cancel();Expect(boot.Poll(18001+24*60*60*1000ull).empty(),"Cancel stops continuous output immediately");
    Expect(!boot.PreparePreset("uboot_space",0,100000000,80),"Unsupported cadence cannot be silently substituted");

}

void TestCarriageReturnAndHistoryRecall() {
    serialctl::TerminalModel model(40, 4);
    model.Feed(L"# ifconfig", L"[10:00:00.001]");
    model.Feed(L"\r\x1b[2K# ls", L"[10:00:01.001]");
    ExpectLine(model, 0, L"# ls", "carriage return and EL replace the current command line");
    Expect(model.DisplayLine(0).timestamp == L"[10:00:00.001]",
        "redrawing a line does not inject a second timestamp");

    model.Feed(L"\r\x1b[2K# ifconfig", L"[10:00:02.001]");
    model.Feed(L"\r\x1b[2K# ls", L"[10:00:03.001]");
    ExpectLine(model, 0, L"# ls", "repeated history navigation keeps one visual line");
}

void TestScrollback() {
    serialctl::TerminalModel model(20, 3, 10);
    model.Feed(L"one\r\ntwo\r\nthree\r\nfour", L"[10:00:00.000]");
    Expect(model.HistorySize() == 1, "scrolling the primary screen creates scrollback");
    ExpectLine(model, 0, L"one", "oldest line is retained in scrollback");
    ExpectLine(model, 3, L"four", "newest line remains on the screen");

    model.Resize(40, 5);
    Expect(model.DisplayLine(0).cells.size() == 40,
        "resizing wider reflows history using the new grid");
    Expect(model.MaximumDisplayColumns() == 40,
        "maximum display width includes the resized live screen");
}

void TestResizePreservesWideHistory() {
    serialctl::TerminalModel model(40, 3, 10);
    model.Feed(L"012345678901234567890123456789\r\nsecond\r\nthird\r\nfourth", L"");
    model.Resize(12, 3);
    Expect(model.PlainText(false) == L"012345678901234567890123456789\r\nsecond\r\nthird\r\nfourth", "shrinking reflows history without modifying hard breaks");
    model.Resize(40,3);
    ExpectLine(model,0,L"012345678901234567890123456789", "growing restores complete historical output");
}

void TestAlternateScreen() {
    serialctl::TerminalModel model(20, 4);
    model.Feed(L"shell prompt", L"[10:00:00.000]");
    model.Feed(L"\x1b[?1049h", L"");
    Expect(model.AlternateScreen(), "DECSET 1049 enters the alternate screen");
    model.Feed(L"vim contents\x1b[2;1Hstatus", L"");
    ExpectLine(model, 0, L"vim contents", "alternate screen accepts positioned output");
    ExpectLine(model, 1, L"status", "CUP positions the alternate-screen cursor");
    model.Feed(L"\x1b[?1049l", L"");
    Expect(!model.AlternateScreen(), "DECRST 1049 leaves the alternate screen");
    ExpectLine(model, 0, L"shell prompt", "leaving alternate screen restores shell contents");
}

void TestEditingAndModes() {
    serialctl::TerminalModel model(20, 4);
    model.Feed(L"abcdef\x1b[3D\x1b[1PXY", L"");
    ExpectLine(model, 0, L"abcXY", "DCH and cursor movement edit cells in place");
    model.Feed(L"\x1b[?1h\x1b[?25l\x1b[?2004h", L"");
    Expect(model.ApplicationCursorKeys(), "application cursor-key mode is tracked");
    Expect(!model.CursorVisible(), "cursor visibility mode is tracked");
    Expect(model.BracketedPaste(), "bracketed paste mode is tracked");
    const auto response = model.Feed(L"\x1b[6n", L"");
    Expect(response.response.find("R") != std::string::npos,
        "cursor position reports are generated for full-screen applications");
}

void TestWideCharactersAndColor() {
    serialctl::TerminalModel model(20, 3);
    model.Feed(L"A中B\x1b[31mR\x1b[0m", L"");
    ExpectLine(model, 0, L"A中BR", "wide characters occupy cells without duplicating text");
    const auto& line = model.DisplayLine(0);
    Expect(line.cells[1].character == L'中' && line.cells[2].continuation,
        "CJK output marks a continuation cell");
    Expect(line.cells[4].attributes.foreground.kind == serialctl::TerminalColor::Kind::Indexed &&
        line.cells[4].attributes.foreground.index == 1,
        "SGR indexed foreground color is stored per cell");
}

void TestFullScreenCompatibilitySequences() {
    serialctl::TerminalModel model(80, 24);
    model.Feed(L"\x1b(0lqqk\x1b(B", L"");
    ExpectLine(model, 0, L"┌──┐", "DEC special graphics render ncurses borders");
    model.Feed(L"\x1b[2;1Habc\x1b[2D\x1b[4hZ\x1b[4l", L"");
    ExpectLine(model, 1, L"aZbc", "insert mode shifts existing cells");
    model.Feed(L"\x1b[3;1HX\x1b[4b", L"");
    ExpectLine(model, 2, L"XXXXX", "REP duplicates the previous character");
    const auto response = model.Feed(L"\x1b[18t", L"");
    Expect(response.response == "\x1b[8;24;80t",
        "terminal-size queries receive rows and columns");

    model.Feed(L"\x1bP$qm\x1b\\visible\x1b_hidden\x1b\\", L"");
    ExpectLine(model, 2, L"XXXXXvisible",
        "DCS and APC control strings are ignored instead of painted as text");
}

void TestUntrustedParameters() {
    serialctl::TerminalModel model(80, 24);
    model.Feed(L"\x1b[-1CX", L"");
    ExpectLine(model, 0, L"X", "negative CSI parameters are rejected");
    model.Feed(L"\x1b[999999999999999999999999CX", L"");
    Expect(model.CursorColumn() >= 0 && model.CursorColumn() < model.Columns(),
        "huge cursor movements remain within the grid");
    model.Feed(L"\x1b[999999999999999999999999AX", L"");
    model.Feed(L"\x1b[999999999999999999999999BX", L"");
    model.Feed(L"\x1b[999999999999999999999999DX", L"");
    model.Feed(L"\x1b[999999999999999999999999XX", L"");
    auto repeated = model.Feed(L"\x1b[999999999999999999999999b", L"");
    Expect(repeated.logText.size() <= 4096, "REP work is bounded");
    for (const wchar_t* command : {L"A", L"B", L"C", L"D", L"E", L"F", L"H", L"X", L"P", L"@"}) {
        model.Feed(std::wstring(L"\x1b[-2147483648") + command + L"Y", L"");
    }
}

void TestLogicalReflowAndBlanks() {
    serialctl::TerminalModel model(20,5);
    auto result=model.Feed(std::wstring(60,L'x'),L"T");
    Expect(result.logText==L"[T] "+std::wstring(60,L'x'),"soft wrapping never enters logs");
    model.Resize(80,5);ExpectLine(model,0,std::wstring(60,L'x'),"20-to-80 joins three automatic wraps");
    Expect(model.CursorColumn()==60,"cursor follows its logical offset after widen");
    for(int i=0;i<20;++i){model.Resize(8,5);model.Resize(80,5);}
    Expect(model.PlainText(false)==std::wstring(60,L'x'),"repeated shrink and grow never truncates active output");
    serialctl::TerminalModel exact(20,5);exact.Feed(L"0123456789ABCDEFGHIJ",L"T");exact.Resize(8,5);exact.Resize(20,5);
    ExpectLine(exact,0,L"0123456789ABCDEFGHIJ","full active line survives narrow grid");exact.Feed(L"Z",L"T");
    ExpectLine(exact,1,L"Z","wrap-pending cursor remains on next output cell");
    serialctl::TerminalModel hard(20,5);auto log=hard.Feed(L"a\r\n\r\nb",L"T");hard.Resize(80,10);
    ExpectLine(hard,0,L"a","hard line preserved");ExpectLine(hard,1,L"","received blank line preserved");ExpectLine(hard,2,L"b","hard lines never joined");
    Expect(hard.DisplayLine(1).timestamp==L"T"&&hard.DisplayLine(1).hardBreak,"true blank has timestamp and hard-break identity");
    Expect(hard.DisplayLine(3).timestamp.empty()&&!hard.DisplayLine(3).hardBreak,"screen padding is not received output");
    Expect(log.logText==L"[T] a\n[T] \n[T] b","log uses identical timestamp policy for a true empty line");
    serialctl::TerminalModel chinese(9,4);chinese.Feed(L"\x1b[31mAB中文中文Z\x1b[0m\r\nnext",L"T");
    const auto before=chinese.PlainText(false);auto anchor=chinese.CaptureAnchor(1,2);
    for(int width:{5,30,7,40})chinese.Resize(width,6);
    Expect(chinese.PlainText(false)==before,"CJK content and hard breaks survive all widths");
    Expect(chinese.DisplayLine(0).cells[2].attributes.foreground.index==1,"SGR attributes survive reflow");
    auto location=chinese.LocateAnchor(anchor);Expect(chinese.CaptureAnchor(location.first,location.second).id==anchor.id,"logical reading/selection anchor survives reflow");
    serialctl::TerminalModel alternate(20,4);alternate.Feed(L"primary",L"T");alternate.Feed(L"\x1b[?1049hTOP\x1b[3;1Hstatus",L"");alternate.Resize(8,4);
    ExpectLine(alternate,2,L"status","alternate grid retains positioned VT rows");alternate.Resize(40,4);alternate.Feed(L"\x1b[?1049l",L"");ExpectLine(alternate,0,L"primary","primary survives alternate resize");
    serialctl::TerminalModel continuous(20,5);std::wstring expected;
    for(int i=0;i<100;++i){continuous.Feed(L"中a",L"T");expected+=L"中a";continuous.Resize(i%2?8:50,5);}
    continuous.Resize(400,5);Expect(continuous.PlainText(false)==expected,"output interleaved with reflow stays intact");
}
void TestHardLineAnchorsAndEditingReflow() {
    for (bool csi : {false, true}) {
        serialctl::TerminalModel saved(20, 10);
        saved.Feed(std::wstring(L"\x1b[8;4H\x1b[31m") +
            (csi ? L"\x1b[s" : L"\x1b" L"7") + L"\x1b[1;1H\x1b[0m", L"");
        saved.Resize(40, 10);
        saved.Feed(csi ? L"\x1b[u" : L"\x1b" L"8", L"");
        Expect(saved.CursorDisplayLine() == 7 && saved.CursorColumn() == 3,
            "saved cursor in an unprinted grid row survives primary reflow");
        Expect(saved.PlainText(false).empty() && saved.DisplayLine(7).timestamp.empty(),
            "saved cursor padding must not become received output");
        saved.Feed(L"X", L"T");
        Expect(saved.DisplayLine(7).cells[3].attributes.foreground.index == 1,
            "saved cursor restores attributes after reflow");
    }
    serialctl::TerminalModel hard(12, 5);
    hard.Feed(L"one\r\ntwo\r\nthree", L"T");
    auto anchor = hard.CaptureAnchor(1, 1);
    Expect(anchor.id != hard.CaptureAnchor(0, 1).id && anchor.id != hard.CaptureAnchor(2, 1).id,
        "distinct hard lines have distinct reading and selection identities");
    hard.Resize(4, 5);
    hard.Resize(20, 5);
    auto location = hard.LocateAnchor(anchor);
    Expect(location.first == 1 && location.second == 1,
        "hard-line reading anchor maps to its own line after repeated resize");
    serialctl::TerminalModel color(4, 4);
    color.Feed(L"abcd\x1b[31mE\x1b[0m", L"T");
    color.Resize(20, 4);
    ExpectLine(color, 0, L"abcdE", "SGR at full-width boundary preserves pending automatic wrap");
    Expect(color.DisplayLine(0).cells[4].attributes.foreground.index == 1,
        "wrapped SGR character keeps its foreground attribute");
    serialctl::TerminalModel deletion(20, 4);
    deletion.Feed(L"abc\x1b[10G\x1b[P\r", L"T");
    deletion.Resize(8, 4);
    deletion.Resize(20, 4);
    ExpectLine(deletion, 0, L"abc", "DCH beyond used text does not truncate preceding logical data");
}
void TestReflowAtScrollbackCapacity() {
    serialctl::TerminalModel model(20, 2, 2);
    model.Feed(L"0123456789ABCDEFGHIJ\r\nsecond\r\nthird\r\nfourth", L"T");
    const auto before = model.PlainText(false);
    const auto anchor = model.CaptureAnchor(0, 12);
    for (int width : {2, 40, 3, 20}) {
        model.Resize(width, 2);
        Expect(model.PlainText(false) == before, "reflow at a full scrollback limit must not evict retained output");
        const auto location = model.LocateAnchor(anchor);
        const auto restored = model.CaptureAnchor(location.first, location.second);
        Expect(restored.id == anchor.id && restored.offset == anchor.offset,
               "reading and selection anchors survive capacity-bound reflow");
    }
    serialctl::TerminalModel hard(20, 2, 2);
    for (int i = 0; i < 100; ++i)
        hard.Feed(L"line\r\n", L"T");
    Expect(hard.HistorySize() <= 2, "new hard-line output still has bounded scrollback");
    serialctl::TerminalModel unbroken(20, 2, 2);
    unbroken.Resize(1, 2);
    unbroken.Feed(std::wstring(1000, L'x'), L"T");
    Expect(unbroken.HistorySize() <= 160, "a never-ending soft-wrapped line is bounded by retained cells");
    serialctl::TerminalModel live(20, 2, 2);
    live.Feed(std::wstring(60, L'x'), L"T");
    live.Resize(2, 2);
    live.Feed(L"中Y", L"T");
    live.Resize(80, 2);
    Expect(live.PlainText(false) == std::wstring(60, L'x') + L"中Y",
           "continuous output during capacity-bound reflow preserves admitted characters");
}
void TestCmdPasteBoundaryMatrix() {
    using Editor = serialctl::CmdLineEditor;
    for (const std::wstring newline : {L"\r", L"\n", L"\r\n"}) {
        for (const size_t caret : {size_t(0), size_t(2), size_t(4)}) {
            for (const size_t tailLength : {size_t(8190), size_t(8191), size_t(8192)}) {
                Editor editor;
                editor.Insert(L"abcd");
                editor.Edit(Editor::Key::Home);
                for (size_t i = 0; i < caret; ++i) editor.Edit(Editor::Key::Right);
                const std::wstring suffix = std::wstring(L"abcd").substr(caret);
                const std::wstring insertedTail(tailLength - suffix.size(), L'x');
                Editor::PastePlan plan;
                const bool accepted = editor.PreparePaste(L"first" + newline + insertedTail, plan);
                Expect(accepted == (tailLength <= 8191),
                       "CR/LF/CRLF and all caret positions enforce the complete tail boundary");
                Expect(editor.Text() == L"abcd" && editor.Cursor() == caret,
                       "every preflight leaves the original draft and caret unchanged");
                if (!accepted) continue;
                const std::wstring line = std::wstring(L"abcd").substr(0, caret) + L"first";
                Expect(plan.submitted == line + newline && plan.tail == insertedTail + suffix &&
                           plan.cursor == insertedTail.size(),
                       "valid boundary paste retains the suffix, caret and exact clipboard newline");
                editor.CommitPaste(plan);
                Expect(editor.Text().size() == tailLength && editor.Cursor() == insertedTail.size(),
                       "accepted boundary paste commits its incomplete draft without adding Enter");
                editor.Edit(Editor::Key::Up);
                Expect(editor.Text() == line, "only the completed line becomes command history");
                editor.Edit(Editor::Key::Down);
                Expect(editor.Text() == insertedTail + suffix, "history restores the boundary draft");
            }
        }
    }
    for (const wchar_t control : {wchar_t(0), wchar_t(8), wchar_t(27), wchar_t(127)}) {
        Editor editor;
        editor.Insert(L"keep");
        editor.Edit(Editor::Key::Left);
        Editor::PastePlan plan;
        Expect(!editor.PreparePaste(L"safe\r\n" + std::wstring(1, control), plan) &&
                   editor.Text() == L"keep" && editor.Cursor() == 3,
               "a forbidden control in a later line rejects the prefix and preserves the draft");
    }
}

void TestCmdEditor() {
    using Plan = serialctl::CmdLineEditor::PastePlan;
    serialctl::CmdLineEditor transactional;
    transactional.Insert(L"echo original");
    Plan invalid;
    Expect(!transactional.PreparePaste(L"\r\necho prefix\r\n" + std::wstring(8192, L'x'), invalid),
           "overlong clipboard tail rejects the entire multi-command paste before submission");
    Expect(transactional.Text() == L"echo original" && transactional.Cursor() == 13,
           "rejected multi-line paste preserves the original draft and caret");
    Expect(!transactional.PreparePaste(L"\r\nvalid\r\n\x1b[A", invalid),
           "interactive control in the tail rejects complete commands too");
    Expect(!transactional.PreparePaste(std::wstring(8192, L'x') + L"\r\nshort", invalid),
           "overlong completed line is rejected before submission");
    transactional.Clear();
    transactional.Insert(L"abcd");
    transactional.Edit(serialctl::CmdLineEditor::Key::Left);
    transactional.Edit(serialctl::CmdLineEditor::Key::Left);
    Plan valid;
    Expect(transactional.PreparePaste(L"first\r\nsecond", valid) &&
               valid.submitted == L"abfirst\r\n" && valid.tail == L"secondcd" && valid.cursor == 6,
           "multi-line paste at caret keeps suffix and correct incomplete-tail caret");
    Expect(transactional.Text() == L"abcd" && transactional.Cursor() == 2,
           "preflight does not change a draft while send has not succeeded");
    transactional.CommitPaste(valid);
    Expect(transactional.Text() == L"secondcd" && transactional.Cursor() == 6,
           "successful submission commits trailing draft exactly once");
    transactional.Edit(serialctl::CmdLineEditor::Key::Up);
    Expect(transactional.Text() == L"abfirst", "only completed paste line enters history");
    transactional.Edit(serialctl::CmdLineEditor::Key::Down);
    Expect(transactional.Text() == L"secondcd", "history restores pasted draft after submission");
    transactional.Clear();
    Expect(transactional.PreparePaste(std::wstring(8191, L'x'), valid) && valid.submitted.empty(),
           "maximum-length paste without newline remains a draft and adds no Enter");
    serialctl::CmdLineEditor pasted;
    pasted.SubmittedBatch(L"echo first\r\necho 中文\n");
    pasted.Insert(L"unsubmitted tail");
    pasted.Edit(serialctl::CmdLineEditor::Key::Up);
    Expect(pasted.Text()==L"echo 中文","clipboard completed Unicode lines enter history");
    pasted.Edit(serialctl::CmdLineEditor::Key::Up);
    Expect(pasted.Text()==L"echo first","clipboard CRLF creates one history entry per command");
    pasted.Edit(serialctl::CmdLineEditor::Key::Down);
    pasted.Edit(serialctl::CmdLineEditor::Key::Down);
    Expect(pasted.Text()==L"unsubmitted tail","history Down restores incomplete clipboard draft");
    serialctl::CmdLineEditor editor;
    for(wchar_t c:std::wstring(L"echo abXd"))editor.Insert(std::wstring(1,c));
    using Key=serialctl::CmdLineEditor::Key;
    editor.Edit(Key::Left);editor.Edit(Key::Backspace);editor.Insert(L"c");
    Expect(editor.Text()==L"echo abcd","CMD Backspace erases the edited draft");
    editor.Edit(Key::Home);editor.Edit(Key::Right);editor.Edit(Key::Delete);editor.Insert(L"c");
    Expect(editor.Text()==L"echo abcd","CMD Delete and insertion edit at caret");
    editor.Edit(Key::End);editor.Insert(L"中文");auto submitted=editor.Text();editor.Submitted();editor.Edit(Key::Up);Expect(editor.Text()==submitted,"CMD history recalls complete Unicode line");
    editor.Edit(Key::Down);Expect(editor.Text().empty(),"history Down restores draft");
    serialctl::TerminalModel output(20,5);output.Feed(L">",L"T");editor.Insert(L"中文abc");
    auto preview=output.PreviewInput(editor.Text(),editor.Cursor());ExpectLine(preview,0,L">中文abc","local preview includes Unicode draft");
    serialctl::TerminalModel longDraft(10,3); longDraft.Feed(L">",L"T");
    auto beginning=longDraft.PreviewInput(std::wstring(200,L'x'),0);
    Expect(beginning.CursorDisplayLine()==0 && beginning.CursorColumn()==1,"Home in long draft preserves visible caret");
    Expect(beginning.LineText(0)==L">xxxxxxxxx","long draft preview shows text at Home caret");
    auto middle=longDraft.PreviewInput(std::wstring(200,L'x'),50);
    Expect(middle.CursorColumn()==1 && middle.CursorDisplayLine()<3,"middle long draft caret remains on screen");
    auto ending=longDraft.PreviewInput(std::wstring(200,L'x'),200);
    Expect(ending.CursorColumn()==1 && ending.CursorDisplayLine()==2,"End in long draft has correct row after scrolling");
    Expect(longDraft.PlainText(false).find(L'x')==std::wstring::npos,"long draft does not mutate stdout");
    output.Feed(L"async\r\n>",L"T");Expect(editor.Text()==L"中文abc","asynchronous output cannot corrupt draft");Expect(output.PlainText(false).find(L"abc")==std::wstring::npos,"unsubmitted draft never contaminates output or logs");
}

} // namespace

int main() {
    {
        // Incremental projection must match a fresh, single-pass VT replay.
        for (const size_t history : {size_t(120), size_t(20000)}) {
            serialctl::TerminalModel incremental(40, 12, history), replay(40, 12, history);
            incremental.SetLineWrapView(false); replay.SetLineWrapView(false);
            std::wstring stream;
            for (int i = 0; i < 600; ++i) stream += L"row " + std::to_wstring(i) + L" " + std::wstring(i % 90, L'x') + L"中文\r\n";
            stream += L"\x1b[?1049h\x1b[H\x1b[2JTUI repaint\x1b[4;3Hcontent\x1b[?1049l\r\nreturned shell";
            for (size_t offset = 0; offset < stream.size(); offset += 37) {
                incremental.Feed(stream.substr(offset, 37), L"T");
                incremental.MaximumDisplayColumns();
                if (incremental.DisplayLineCount()) incremental.LineText(incremental.DisplayLineCount() - 1);
            }
            replay.Feed(stream, L"T");
            Expect(incremental.PlainText(false) == replay.PlainText(false), "incremental history cache changed terminal contents");
            Expect(incremental.DisplayLineCount() == replay.DisplayLineCount() &&
                   incremental.MaximumDisplayColumns() == replay.MaximumDisplayColumns(),
                   "incremental projection width/count diverged after trim or TUI exit");
            incremental.Resize(55, 14); replay.Resize(55, 14);
            Expect(incremental.PlainText(false) == replay.PlainText(false), "resizing reused stale terminal projection");
            incremental.ClearScrollback(); replay.ClearScrollback();
            incremental.Feed(L"\r\nnew", L"T"); replay.Feed(L"\r\nnew", L"T");
            Expect(incremental.PlainText(false) == replay.PlainText(false), "clearing history reused stale projection");
        }
    }
    {
        serialctl::TerminalModel view(5, 6);
        view.Feed(L"ABC中文DEF\r\nHARD", L"device source");
        const auto raw = view.PlainText(false);
        const auto columns = view.Columns();
        view.SetLineWrapView(false);
        ExpectLine(view, 0, L"ABC中文DEF", "long-line projection joins only soft wraps including wide characters");
        ExpectLine(view, 1, L"HARD", "explicit CR/LF survives long-line projection");
        Expect(view.Columns() == columns && view.MaximumDisplayColumns() >= 10,
               "long-line view does not widen parser or PTY dimensions");
        Expect(view.SelectedText(0, 0, 1, 3) == L"ABC中文DEF\r\nHARD", "projection copy preserves raw hard breaks");
        const auto anchor = view.CaptureAnchor(0, 7);
        view.Resize(8, 6);
        ExpectLine(view, 0, L"ABC中文DEF", "long line survives window reflow");
        Expect(view.LocateAnchor(anchor) == std::make_pair<size_t, int>(0, 7),
               "reading anchor survives projection and reflow");
        view.Feed(L" MORE", L"new");
        ExpectLine(view, 1, L"HARD MORE", "new output invalidates projected line cache");
        auto draft = view.PreviewInput(L"123456789abcdefgh", 5);
        Expect(draft.PlainText(false).find(L"HARD MORE123456789abcdefgh") != std::wstring::npos,
               "CMD preview retains history and long line text");
        Expect(view.PlainText(false).find(L"123456789") == std::wstring::npos,
               "CMD preview cannot mutate real device evidence");
        view.Feed(L"\x1b[?1049h123456789", L"");
        Expect(view.AlternateScreen() && view.MaximumDisplayColumns() == 8, "alternate screen keeps native grid width");
        view.Feed(L"\x1b[?1049l", L"");
        ExpectLine(view, 0, L"ABC中文DEF", "leaving full screen restores long-line view");
        view.SetLineWrapView(true);
        Expect(view.PlainText(false).find(L"HARD MORE") != std::wstring::npos,
               "wrap toggle preserves terminal evidence");
    }
    TestSharedResponseOwnership();
    serialctl::TerminalModel selection(5, 5);
    selection.Feed(L"abc  def\r\n中 x ", L"timestamp");
    Expect(selection.SelectedText(0, 0, 1, 2) == L"abc  def",
           "copy preserves real spaces at a soft wrap without adding a hard newline");
    Expect(selection.SelectedText(2, 4, 0, 0) == L"abc  def\r\n中 x ",
           "reverse selection preserves written trailing spaces and skips wide continuation cells");
    Expect(selection.SelectedText(1, 3, 1, 4).empty(),
           "selection of unwritten padding has no clipboard content");
    selection.Feed(L"\x1b[?1049hAB  C\r\nD", L"");
    Expect(selection.SelectedText(0, 0, 1, 0) == L"AB  C\r\nD",
           "alternate-screen selection preserves grid hard breaks and spaces");
    serialctl::TerminalModel osc;
    auto cwd = osc.Feed(L"\x1b]7;file://host/tmp/a%20b/%E4%B8%AD\a", L"");
    Expect(cwd.workingDirectory == L"/tmp/a b/中", "OSC7 percent UTF8 working directory");
    Expect(osc.Feed(L"\x1b]7;file://host/var", L"").workingDirectory.empty(), "split OSC7 retained");
    Expect(osc.Feed(L"/log\x1b\\", L"").workingDirectory == L"/var/log", "OSC7 ST terminator");
    Expect(osc.Feed(L"\x1b]7;file://host/%00\a", L"").workingDirectory.empty(), "invalid path control rejected");
    Expect(osc.Feed(L"\x1b]7;file://host/%C0%AF\a", L"").workingDirectory.empty(), "overlong UTF8 rejected");
    Expect(osc.Feed(std::wstring(L"\x1b]7;file://host/") + std::wstring(1200, L'a') + L"\a", L"").workingDirectory.empty(), "overlong OSC7 ignored rather than truncated");
    serialctl::TerminalModel resizing(40, 24);
    resizing.Feed(L"only data", L"10:00:00.000");
    for (int i=0;i<50;++i) { resizing.Resize(40, 5); resizing.Resize(40, 24); }
    Expect(resizing.HistorySize()==0, "unused bottom rows never become empty history");
    ExpectLine(resizing,0,L"only data","resizing idle terminal preserves output position");
    serialctl::TerminalModel cursorResize(40,4);
    cursorResize.Feed(L"one\r\ntwo\r\nthree\r\nfour", L"10:00:00.000");
    cursorResize.Resize(40,2); cursorResize.Resize(40,4);
    Expect(cursorResize.HistorySize()==0,"idle grow restores rows moved by shrink");
    ExpectLine(cursorResize,3,L"four","cursor row survives shrink then grow");
    TestEnhancedTuiInputAndExit();
    TestLocalBootInterrupt();
    TestLogicalReflowAndBlanks();
    TestHardLineAnchorsAndEditingReflow();
    TestReflowAtScrollbackCapacity();
    TestCmdEditor();
    TestCmdPasteBoundaryMatrix();
    TestUntrustedParameters();
    TestCarriageReturnAndHistoryRecall();
    TestScrollback();
    TestResizePreservesWideHistory();
    TestAlternateScreen();
    TestEditingAndModes();
    TestWideCharactersAndColor();
    TestFullScreenCompatibilitySequences();
    if (failures == 0) std::cout << "Terminal model tests passed\n";
    return failures == 0 ? 0 : 1;
}
