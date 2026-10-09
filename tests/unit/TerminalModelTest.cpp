#include "TerminalModel.h"

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
    Expect(model.DisplayLine(0).cells.size() == 20,
        "resizing wider keeps historical lines at their original safe width");
    Expect(model.MaximumDisplayColumns() == 40,
        "maximum display width includes the resized live screen");
}

void TestResizePreservesWideHistory() {
    serialctl::TerminalModel model(40, 3, 10);
    model.Feed(L"012345678901234567890123456789\r\nsecond\r\nthird\r\nfourth", L"");
    model.Resize(12, 3);
    Expect(model.DisplayLine(0).cells.size() == 40,
        "shrinking the terminal preserves wide scrollback for horizontal review");
    Expect(model.LineText(0) == L"012345678901234567890123456789",
        "shrinking does not truncate historical output");
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

} // namespace

int main() {
    serialctl::TerminalModel osc;
    auto cwd = osc.Feed(L"\x1b]7;file://host/tmp/a%20b/%E4%B8%AD\a", L"");
    Expect(cwd.workingDirectory == L"/tmp/a b/中", "OSC7 percent UTF8 working directory");
    Expect(osc.Feed(L"\x1b]7;file://host/var", L"").workingDirectory.empty(), "split OSC7 retained");
    Expect(osc.Feed(L"/log\x1b\\", L"").workingDirectory == L"/var/log", "OSC7 ST terminator");
    Expect(osc.Feed(L"\x1b]7;file://host/%00\a", L"").workingDirectory.empty(), "invalid path control rejected");
    Expect(osc.Feed(L"\x1b]7;file://host/%C0%AF\a", L"").workingDirectory.empty(), "overlong UTF8 rejected");
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
