#include "TerminalModel.h"
#include "CmdLineEditor.h"

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
void TestCmdEditor() {
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
    TestLogicalReflowAndBlanks();
    TestHardLineAnchorsAndEditingReflow();
    TestCmdEditor();
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
