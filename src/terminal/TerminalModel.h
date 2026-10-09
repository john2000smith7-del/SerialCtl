#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace serialctl {

struct TerminalColor {
    enum class Kind : std::uint8_t { Default, Indexed, Rgb };

    Kind kind = Kind::Default;
    std::uint8_t index = 0;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    bool operator==(const TerminalColor& other) const;
    bool operator!=(const TerminalColor& other) const { return !(*this == other); }
};

struct TerminalAttributes {
    TerminalColor foreground;
    TerminalColor background;
    bool bold = false;
    bool underline = false;
    bool inverse = false;

    bool operator==(const TerminalAttributes& other) const;
    bool operator!=(const TerminalAttributes& other) const { return !(*this == other); }
};

struct TerminalCell {
    wchar_t character = L' ';
    TerminalAttributes attributes;
    bool continuation = false;
};

struct TerminalLine {
    std::vector<TerminalCell> cells;
    std::wstring timestamp;
    bool wrapped = false;
};

struct TerminalFeedResult {
    std::string response;
    std::wstring logText;
    size_t scrollbackAdded = 0;
    std::wstring workingDirectory;
    bool titleChanged = false;
    std::wstring title;
};

class TerminalModel {
public:
    explicit TerminalModel(int columns = 80, int rows = 24, size_t maximumScrollback = 20000);

    TerminalFeedResult Feed(const std::wstring& text, const std::wstring& timestamp);
    void Resize(int columns, int rows);
    void Reset();
    void Clear();
    void ClearScrollback();

    int Columns() const { return columns_; }
    int Rows() const { return rows_; }
    size_t HistorySize() const;
    size_t DisplayLineCount() const;
    size_t MaximumDisplayColumns() const;
    const TerminalLine& DisplayLine(size_t index) const;
    size_t CursorDisplayLine() const;
    int CursorColumn() const;
    bool CursorVisible() const { return cursorVisible_; }
    bool AlternateScreen() const { return alternateScreen_; }
    bool ApplicationCursorKeys() const { return applicationCursorKeys_; }
    bool BracketedPaste() const { return bracketedPaste_; }

    std::wstring LineText(size_t displayLine, bool trimRight = true) const;
    std::wstring CurrentLineText() const;
    std::wstring PlainText(bool includeTimestamps) const;

private:
    enum class ParserState {
        Ground,
        Escape,
        EscapeIntermediate,
        Csi,
        Osc,
        OscEscape,
        StringIgnore,
        StringIgnoreEscape
    };

    struct Screen {
        std::vector<TerminalLine> lines;
        int cursorRow = 0;
        int cursorColumn = 0;
        int savedRow = 0;
        int savedColumn = 0;
        int scrollTop = 0;
        int scrollBottom = 0;
        bool wrapPending = false;
    };

    TerminalLine BlankLine() const;
    Screen BlankScreen() const;
    Screen& ActiveScreen();
    const Screen& ActiveScreen() const;
    void ResizeScreen(Screen& screen, bool preserveHistory, int columns, int rows);
    void PutCharacter(wchar_t character, const std::wstring& timestamp, TerminalFeedResult& result);
    void ControlCharacter(
        wchar_t character, const std::wstring& timestamp, TerminalFeedResult& result);
    void LineFeed(TerminalFeedResult& result);
    void ReverseIndex();
    void ScrollUp(int count, TerminalFeedResult* result = nullptr);
    void ScrollDown(int count);
    void EraseDisplay(int mode);
    void EraseLine(int mode);
    void EraseCharacters(int count);
    void InsertCharacters(int count);
    void DeleteCharacters(int count);
    void InsertLines(int count);
    void DeleteLines(int count);
    void HandleOsc(TerminalFeedResult& result);
    void ExecuteEscape(wchar_t character, TerminalFeedResult& result);
    void ExecuteCsi(wchar_t finalCharacter, TerminalFeedResult& result);
    void SetGraphicsRendition(const std::vector<int>& parameters);
    void SetPrivateMode(int mode, bool enabled);
    void SwitchAlternateScreen(bool enabled, bool saveCursor);
    void AppendLogCharacter(
        wchar_t character, const std::wstring& timestamp, TerminalFeedResult& result);
    std::vector<int> ParseParameters(std::wstring text, wchar_t& prefix) const;
    int Parameter(const std::vector<int>& parameters, size_t index, int fallback) const;
    void EnsureTimestamp(TerminalLine& line, const std::wstring& timestamp);
    static bool IsWideCharacter(wchar_t character);
    static bool IsCombiningCharacter(wchar_t character);
    wchar_t MapSpecialGraphics(wchar_t character) const;

    int columns_ = 80;
    int rows_ = 24;
    size_t maximumScrollback_ = 20000;
    std::deque<TerminalLine> history_;
    Screen primary_;
    Screen alternate_;
    bool alternateScreen_ = false;
    bool cursorVisible_ = true;
    bool applicationCursorKeys_ = false;
    bool bracketedPaste_ = false;
    bool autoWrap_ = true;
    bool originMode_ = false;
    bool insertMode_ = false;
    bool decSpecialGraphics_ = false;
    wchar_t escapeIntermediate_ = 0;
    wchar_t lastPrintedCharacter_ = 0;
    TerminalAttributes attributes_;
    TerminalAttributes savedAttributes_;
    ParserState parserState_ = ParserState::Ground;
    std::wstring csiBuffer_;
    std::wstring oscBuffer_;
    bool logAtLineStart_ = true;
};

} // namespace serialctl
