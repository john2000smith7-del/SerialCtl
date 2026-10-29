#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include <utility>
#include <memory>

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
    bool hardBreak = false;
    size_t used = 0;
    std::uint64_t logicalId = 0;
    size_t logicalOffset = 0;
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
    enum class ResponseRole { Owner, Viewer };
    explicit TerminalModel(int columns = 80, int rows = 24, size_t maximumScrollback = 20000);

    // The connection owner answers device queries. Shared live/history viewers
    // only render them; Reset, Clear and remote RIS must not change ownership.
    void SetResponseRole(ResponseRole role) { responseRole_ = role; }
    TerminalFeedResult Feed(const std::wstring& text, const std::wstring& timestamp);
    void Resize(int columns, int rows);
    // Display-only projection: never changes PTY dimensions or VT parsing.
    void SetLineWrapView(bool enabled);
    bool LineWrapView() const {
        return lineWrapView_;
    }
    struct Anchor {
        std::uint64_t id = 0;
        size_t offset = 0;
    };
    Anchor CaptureAnchor(size_t line, int column = 0) const;
    std::pair<size_t, int> LocateAnchor(Anchor) const;
    TerminalModel PreviewInput(const std::wstring &text, size_t cursor) const;
    void Reset();
    void Clear();
    void ClearScrollback();

    std::uint64_t Revision() const { return revision_; }
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
    int CursorRow() const { return ActiveScreen().cursorRow; }
    std::wstring ScreenText(size_t maximumRows = 200) const;
    unsigned KeyboardFlags() const { return ActiveScreen().keyboardFlags; }
    bool MouseCaptured() const { return mouseTracking_ != 0; }
    int MouseTracking() const { return mouseTracking_; }
    bool AlternateScroll() const { return alternateScroll_; }
    // Shared by real keyboard input and bounded AI key requests. Modifier bits
    // are Shift=1, Alt=2, Ctrl=4; unknown names return no bytes.
    std::string KeySequence(std::string name, unsigned modifiers = 0) const;
    std::string WheelSequence(bool up, int column, int row, unsigned modifiers = 0) const;
    std::string MouseSequence(int button, int column, int row, unsigned modifiers = 0, bool release = false) const;

    std::wstring LineText(size_t displayLine, bool trimRight = true) const;
    std::wstring CurrentLineText() const;
    std::wstring PlainText(bool includeTimestamps) const;
    // Inclusive cell endpoints; preserve written spaces and hard line breaks,
    // but omit unused padding and wide-character continuation cells.
    std::wstring SelectedText(size_t firstLine, int firstColumn, size_t lastLine, int lastColumn) const;

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
        TerminalAttributes savedAttributes;
        unsigned keyboardFlags = 0;
        std::vector<unsigned> keyboardStack;
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
    void ClearHistory();
    void RecountHistory();
    void TrimHistory();
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

    const TerminalLine &RawDisplayLine(size_t index) const;
    void EnsureProjection() const;
    bool lineWrapView_ = true;
    mutable bool projectionDirty_ = true;
    mutable std::vector<size_t> projectionStarts_;
    mutable std::vector<std::shared_ptr<const TerminalLine>> projectedLines_;
    mutable std::vector<bool> projectionReady_;
    mutable std::vector<size_t> projectionMaximum_;
    std::uint64_t revision_ = 0;
    std::uint64_t historyRevision_ = 0;
    mutable std::uint64_t projectedHistoryRevision_ = ~std::uint64_t(0);
    mutable size_t projectedHistorySize_ = 0;
    mutable std::uint64_t nextLogicalId_ = 1;
    int columns_ = 80;
    int rows_ = 24;
    size_t maximumScrollback_ = 20000;
    size_t maximumScrollbackCells_ = 1600000;
    size_t historyCells_ = 0;
    size_t historyLogicalLines_ = 0;
    // Historical cells are immutable; snapshots share them without copying cell buffers.
    std::deque<std::shared_ptr<const TerminalLine>> history_;
    size_t resizeHistoryLines_ = 0;
    Screen primary_;
    Screen alternate_;
    bool alternateScreen_ = false;
    ResponseRole responseRole_ = ResponseRole::Owner;
    bool cursorVisible_ = true;
    bool applicationCursorKeys_ = false;
    bool bracketedPaste_ = false;
    int mouseTracking_ = 0;
    bool sgrMouse_ = false, alternateScroll_ = true;
    unsigned primaryModes_ = 0;
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
    bool oscOverflow_ = false;
    bool logAtLineStart_ = true;
};

} // namespace serialctl
