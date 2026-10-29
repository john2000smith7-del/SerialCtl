#include "TerminalModel.h"

#include <algorithm>
#include <cwctype>
#include <limits>

namespace serialctl {

namespace {

TerminalColor IndexedColor(int index) {
    TerminalColor color;
    color.kind = TerminalColor::Kind::Indexed;
    color.index = static_cast<std::uint8_t>(std::max(0, std::min(255, index)));
    return color;
}

TerminalColor RgbColor(int red, int green, int blue) {
    TerminalColor color;
    color.kind = TerminalColor::Kind::Rgb;
    color.red = static_cast<std::uint8_t>(std::max(0, std::min(255, red)));
    color.green = static_cast<std::uint8_t>(std::max(0, std::min(255, green)));
    color.blue = static_cast<std::uint8_t>(std::max(0, std::min(255, blue)));
    return color;
}

} // namespace

bool TerminalColor::operator==(const TerminalColor& other) const {
    return kind == other.kind && index == other.index && red == other.red &&
        green == other.green && blue == other.blue;
}

bool TerminalAttributes::operator==(const TerminalAttributes& other) const {
    return foreground == other.foreground && background == other.background &&
        bold == other.bold && underline == other.underline && inverse == other.inverse;
}

TerminalModel::TerminalModel(int columns, int rows, size_t maximumScrollback)
    : columns_(std::max(1, columns)),
      rows_(std::max(1, rows)),
      maximumScrollback_(std::max<size_t>(1, maximumScrollback)) {
    // The retention budget is stable across resizes. At the default 80-column
    // grid it holds 20,000 logical lines or 1.6 million cells, whichever fills
    // first. A long stream without hard breaks must also remain bounded.
    const auto budgetColumns = static_cast<size_t>(std::max(80, columns_));
    maximumScrollbackCells_ =
        std::min(maximumScrollback_, std::numeric_limits<size_t>::max() / budgetColumns) * budgetColumns;
    primary_ = BlankScreen();
    alternate_ = BlankScreen();
}

TerminalLine TerminalModel::BlankLine() const {
    TerminalLine line;
    line.logicalId = nextLogicalId_++;
    line.cells.resize(static_cast<size_t>(columns_));
    return line;
}

TerminalModel::Screen TerminalModel::BlankScreen() const {
    Screen screen;
    screen.lines.reserve(static_cast<size_t>(rows_));
    for (int row = 0; row < rows_; ++row)
        screen.lines.push_back(BlankLine());
    screen.scrollBottom = rows_ - 1;
    return screen;
}

TerminalModel::Screen& TerminalModel::ActiveScreen() {
    return alternateScreen_ ? alternate_ : primary_;
}

const TerminalModel::Screen& TerminalModel::ActiveScreen() const {
    return alternateScreen_ ? alternate_ : primary_;
}

void TerminalModel::HandleOsc(TerminalFeedResult& result) {
    if (oscOverflow_) return;
    const size_t separator = oscBuffer_.find(L';');
    if (separator == std::wstring::npos) return;
    const auto kind = oscBuffer_.substr(0, separator);
    const auto value = oscBuffer_.substr(separator + 1);
    if (kind == L"0" || kind == L"2") { result.title = value; result.titleChanged = true; return; }
    if (kind != L"7" || value.rfind(L"file://", 0) != 0) return;
    const size_t slash = value.find(L'/', 7);
    if (slash == std::wstring::npos) return;
    std::wstring path;
    auto hex = [](wchar_t c) -> int { if (c >= L'0' && c <= L'9') return c - L'0'; if (c >= L'a' && c <= L'f') return c - L'a' + 10; if (c >= L'A' && c <= L'F') return c - L'A' + 10; return -1; };
    for (size_t i = slash; i < value.size();) {
        if (value[i] != L'%') { path += value[i++]; continue; }
        std::vector<unsigned char> bytes;
        while (i < value.size() && value[i] == L'%') {
            if (i + 2 >= value.size() || hex(value[i+1]) < 0 || hex(value[i+2]) < 0) return;
            bytes.push_back(static_cast<unsigned char>((hex(value[i+1]) << 4) | hex(value[i+2]))); i += 3;
        }
        for (size_t j = 0; j < bytes.size();) {
            unsigned code = bytes[j++], count = 0, minimum = 0;
            if (code < 128) {}
            else if (code >= 0xc2 && code <= 0xdf) { code &= 31; count = 1; minimum = 128; }
            else if (code >= 0xe0 && code <= 0xef) { code &= 15; count = 2; minimum = 2048; }
            else if (code >= 0xf0 && code <= 0xf4) { code &= 7; count = 3; minimum = 65536; }
            else return;
            if (j + count > bytes.size()) return;
            while (count--) { if ((bytes[j] & 0xc0) != 0x80) return; code = (code << 6) | (bytes[j++] & 63); }
            if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return;
            if (sizeof(wchar_t) == 2 && code > 0xffff) { code -= 0x10000; path += static_cast<wchar_t>(0xd800 + (code >> 10)); path += static_cast<wchar_t>(0xdc00 + (code & 1023)); }
            else path += static_cast<wchar_t>(code);
        }
    }
    if (path.empty() || path.front() != L'/') return;
    for (wchar_t c : path) if (c < 32 || c == 127) return;
    result.workingDirectory = std::move(path);
}

TerminalFeedResult TerminalModel::Feed(const std::wstring& text, const std::wstring& timestamp) {
    ++revision_;
    const auto oldDisplayCount = !lineWrapView_ && !alternateScreen_ ? DisplayLineCount() : 0;
    projectionDirty_ = true;
    if (!text.empty()) resizeHistoryLines_ = 0;
    TerminalFeedResult result;
    for (const wchar_t character : text) {
        switch (parserState_) {
        case ParserState::Ground:
            if (character == 0x9b) {
                csiBuffer_.clear();
                parserState_ = ParserState::Csi;
            } else if (character == 0x9d) {
                oscBuffer_.clear(); oscOverflow_ = false;
                parserState_ = ParserState::Osc;
            } else if (character == 0x90 || character == 0x98 ||
                character == 0x9e || character == 0x9f) {
                parserState_ = ParserState::StringIgnore;
            } else if (character == 0x1b) {
                parserState_ = ParserState::Escape;
            } else if (character < 0x20 || character == 0x7f) {
                ControlCharacter(character, timestamp, result);
            } else {
                PutCharacter(character, timestamp, result);
                AppendLogCharacter(character, timestamp, result);
            }
            break;
        case ParserState::Escape:
            if (character == L'[') {
                csiBuffer_.clear();
                parserState_ = ParserState::Csi;
            } else if (character == L']') {
                oscBuffer_.clear(); oscOverflow_ = false;
                parserState_ = ParserState::Osc;
            } else if (character == L'P' || character == L'X' ||
                character == L'^' || character == L'_') {
                parserState_ = ParserState::StringIgnore;
            } else if (character == L'(' || character == L')' || character == L'*' || character == L'+') {
                escapeIntermediate_ = character;
                parserState_ = ParserState::EscapeIntermediate;
            } else {
                ExecuteEscape(character, result);
                parserState_ = ParserState::Ground;
            }
            break;
        case ParserState::EscapeIntermediate:
            if (escapeIntermediate_ == L'(')
                decSpecialGraphics_ = character == L'0';
            escapeIntermediate_ = 0;
            parserState_ = ParserState::Ground;
            break;
        case ParserState::Csi:
            if (character >= 0x40 && character <= 0x7e) {
                ExecuteCsi(character, result);
                csiBuffer_.clear();
                parserState_ = ParserState::Ground;
            } else if (character == 0x1b) {
                parserState_ = ParserState::Escape;
            } else if (csiBuffer_.size() < 128) {
                csiBuffer_ += character;
            }
            break;
        case ParserState::Osc:
            if (character == L'\a' || character == 0x9c) {
                HandleOsc(result);
                oscBuffer_.clear(); oscOverflow_ = false;
                parserState_ = ParserState::Ground;
            } else if (character == 0x1b) {
                parserState_ = ParserState::OscEscape;
            } else if (oscBuffer_.size() < 1024) {
                oscBuffer_ += character;
            } else { oscOverflow_ = true; }
            break;
        case ParserState::OscEscape:
            if (character == L'\\') {
                HandleOsc(result);
                oscBuffer_.clear(); oscOverflow_ = false;
                parserState_ = ParserState::Ground;
            } else {
                parserState_ = ParserState::Osc;
            }
            break;
        case ParserState::StringIgnore:
            if (character == 0x1b)
                parserState_ = ParserState::StringIgnoreEscape;
            else if (character == 0x9c)
                parserState_ = ParserState::Ground;
            break;
        case ParserState::StringIgnoreEscape:
            parserState_ = character == L'\\' ? ParserState::Ground : ParserState::StringIgnore;
            break;
        }
    }
    if (!lineWrapView_ && !alternateScreen_) {
        projectionDirty_ = true;
        const auto count = DisplayLineCount();
        result.scrollbackAdded = count > oldDisplayCount ? count - oldDisplayCount : 0;
    }
    return result;
}

void TerminalModel::PutCharacter(
    wchar_t character, const std::wstring& timestamp, TerminalFeedResult& result) {
    Screen& screen = ActiveScreen();
    character = MapSpecialGraphics(character);
    const int width = IsWideCharacter(character) ? 2 : 1;
    if (screen.wrapPending || screen.cursorColumn + width > columns_) {
        if (autoWrap_) {
            auto &previous = screen.lines[static_cast<size_t>(screen.cursorRow)];
            previous.wrapped = true;
            const auto id = previous.logicalId;
            const auto offset = previous.logicalOffset + previous.used;
            screen.cursorColumn = 0;
            screen.wrapPending = false;
            LineFeed(result);
            auto &continuation = screen.lines[static_cast<size_t>(screen.cursorRow)];
            continuation.logicalId = id;
            continuation.logicalOffset = offset;
        } else {
            screen.cursorColumn = columns_ - 1;
            screen.wrapPending = false;
        }
    }

    TerminalLine& line = screen.lines[static_cast<size_t>(screen.cursorRow)];
    if (line.logicalOffset == 0)
        EnsureTimestamp(line, timestamp);
    if (IsCombiningCharacter(character) && screen.cursorColumn > 0) {
        // Win32 GDI does not provide a stable cell for combining marks. Keeping the
        // base character avoids shifting every cell that follows it.
        return;
    }
    const size_t column = static_cast<size_t>(screen.cursorColumn);
    if (insertMode_) InsertCharacters(width);
    line.cells[column].character = character;
    line.cells[column].attributes = attributes_;
    line.cells[column].continuation = false;
    if (width == 2 && column + 1 < line.cells.size()) {
        line.cells[column + 1].character = L' ';
        line.cells[column + 1].attributes = attributes_;
        line.cells[column + 1].continuation = true;
    }
    line.used = std::max(line.used, column + static_cast<size_t>(width));
    screen.cursorColumn += width;
    if (screen.cursorColumn >= columns_) {
        screen.cursorColumn = columns_ - 1;
        screen.wrapPending = autoWrap_;
    }
    lastPrintedCharacter_ = character;
}

void TerminalModel::ControlCharacter(
    wchar_t character, const std::wstring& timestamp, TerminalFeedResult& result) {
    Screen& screen = ActiveScreen();
    switch (character) {
    case L'\r':
        screen.cursorColumn = 0;
        screen.wrapPending = false;
        AppendLogCharacter(character, timestamp, result);
        break;
    case L'\n':
    case L'\v':
    case L'\f':
        EnsureTimestamp(screen.lines[static_cast<size_t>(screen.cursorRow)], timestamp);
        screen.lines[static_cast<size_t>(screen.cursorRow)].hardBreak = true;
        screen.lines[static_cast<size_t>(screen.cursorRow)].wrapped = false;
        LineFeed(result);
        AppendLogCharacter(L'\n', timestamp, result);
        break;
    case L'\b':
    case 0x7f:
        if (screen.cursorColumn > 0) --screen.cursorColumn;
        screen.wrapPending = false;
        break;
    case L'\t': {
        const int next = std::min(columns_ - 1, (screen.cursorColumn / 8 + 1) * 8);
        screen.cursorColumn = next;
        break;
    }
    case L'\a':
    case L'\0':
    default:
        break;
    }
}

void TerminalModel::LineFeed(TerminalFeedResult& result) {
    Screen& screen = ActiveScreen();
    screen.wrapPending = false;
    if (screen.cursorRow == screen.scrollBottom) {
        ScrollUp(1, &result);
    } else if (screen.cursorRow < rows_ - 1) {
        ++screen.cursorRow;
    }
}

void TerminalModel::ReverseIndex() {
    Screen& screen = ActiveScreen();
    screen.wrapPending = false;
    if (screen.cursorRow == screen.scrollTop) ScrollDown(1);
    else if (screen.cursorRow > 0) --screen.cursorRow;
}

void TerminalModel::ScrollUp(int count, TerminalFeedResult* result) {
    Screen& screen = ActiveScreen();
    count = std::max(1, std::min(count, screen.scrollBottom - screen.scrollTop + 1));
    for (int iteration = 0; iteration < count; ++iteration) {
        if (!alternateScreen_ && screen.scrollTop == 0 && screen.scrollBottom == rows_ - 1) {
            const auto &line = screen.lines.front();
            historyCells_ += std::max<size_t>(1, line.used);
            if (history_.empty() || history_.back()->logicalId != line.logicalId)
                ++historyLogicalLines_;
            history_.push_back(std::make_shared<const TerminalLine>(std::move(screen.lines.front())));
            TrimHistory();
            if (result) ++result->scrollbackAdded;
        }
        screen.lines.erase(screen.lines.begin() + screen.scrollTop);
        screen.lines.insert(screen.lines.begin() + screen.scrollBottom, BlankLine());
    }
}

void TerminalModel::ClearHistory() {
    ++historyRevision_;
    history_.clear();
    historyCells_ = 0;
    historyLogicalLines_ = 0;
}

void TerminalModel::RecountHistory() {
    ++historyRevision_;
    historyCells_ = 0;
    historyLogicalLines_ = 0;
    std::uint64_t previous = 0;
    for (const auto &entry : history_) {
        const auto& line = *entry;
        historyCells_ += std::max<size_t>(1, line.used);
        if (line.logicalId != previous)
            ++historyLogicalLines_;
        previous = line.logicalId;
    }
}

void TerminalModel::TrimHistory() {
    while (!history_.empty() &&
           (historyLogicalLines_ > maximumScrollback_ || historyCells_ > maximumScrollbackCells_)) {
        historyCells_ -= std::max<size_t>(1, history_.front()->used);
        const auto id = history_.front()->logicalId;
        history_.pop_front();
        ++historyRevision_;
        if (history_.empty() || history_.front()->logicalId != id)
            --historyLogicalLines_;
    }
}

void TerminalModel::ScrollDown(int count) {
    Screen& screen = ActiveScreen();
    count = std::max(1, std::min(count, screen.scrollBottom - screen.scrollTop + 1));
    for (int iteration = 0; iteration < count; ++iteration) {
        screen.lines.erase(screen.lines.begin() + screen.scrollBottom);
        screen.lines.insert(screen.lines.begin() + screen.scrollTop, BlankLine());
    }
}

void TerminalModel::EraseDisplay(int mode) {
    Screen& screen = ActiveScreen();
    if (mode == 2 || mode == 3) {
        for (TerminalLine& line : screen.lines) line = BlankLine();
        if (mode == 3)
            ClearHistory();
        return;
    }
    if (mode == 0) {
        EraseLine(0);
        for (int row = screen.cursorRow + 1; row < rows_; ++row)
            screen.lines[static_cast<size_t>(row)] = BlankLine();
    } else if (mode == 1) {
        EraseLine(1);
        for (int row = 0; row < screen.cursorRow; ++row)
            screen.lines[static_cast<size_t>(row)] = BlankLine();
    }
}

void TerminalModel::EraseLine(int mode) {
    Screen& screen = ActiveScreen();
    TerminalLine& line = screen.lines[static_cast<size_t>(screen.cursorRow)];
    int first = 0;
    int last = columns_ - 1;
    if (mode == 0) first = screen.cursorColumn;
    else if (mode == 1) last = screen.cursorColumn;
    for (int column = first; column <= last; ++column)
        line.cells[static_cast<size_t>(column)] = TerminalCell{};
    if (mode == 0)
        line.used = std::min(line.used, static_cast<size_t>(first));
    if (mode == 2) {
        line.used = 0;
        line.wrapped = false;
    }
}

void TerminalModel::EraseCharacters(int count) {
    Screen& screen = ActiveScreen();
    TerminalLine& line = screen.lines[static_cast<size_t>(screen.cursorRow)];
    const int last = std::min(columns_, screen.cursorColumn + std::max(1, count));
    for (int column = screen.cursorColumn; column < last; ++column)
        line.cells[static_cast<size_t>(column)] = TerminalCell{};
}

void TerminalModel::InsertCharacters(int count) {
    Screen& screen = ActiveScreen();
    TerminalLine& line = screen.lines[static_cast<size_t>(screen.cursorRow)];
    count = std::max(1, std::min(count, columns_ - screen.cursorColumn));
    if (line.used > static_cast<size_t>(screen.cursorColumn))
        line.used = std::min(static_cast<size_t>(columns_), line.used + count);
    std::move_backward(line.cells.begin() + screen.cursorColumn,
        line.cells.end() - count, line.cells.end());
    std::fill(line.cells.begin() + screen.cursorColumn,
        line.cells.begin() + screen.cursorColumn + count, TerminalCell{});
}

void TerminalModel::DeleteCharacters(int count) {
    Screen& screen = ActiveScreen();
    TerminalLine& line = screen.lines[static_cast<size_t>(screen.cursorRow)];
    count = std::max(1, std::min(count, columns_ - screen.cursorColumn));
    if (line.used > static_cast<size_t>(screen.cursorColumn))
        line.used -= std::min(static_cast<size_t>(count), line.used - static_cast<size_t>(screen.cursorColumn));
    std::move(line.cells.begin() + screen.cursorColumn + count,
        line.cells.end(), line.cells.begin() + screen.cursorColumn);
    std::fill(line.cells.end() - count, line.cells.end(), TerminalCell{});
}

void TerminalModel::InsertLines(int count) {
    Screen& screen = ActiveScreen();
    if (screen.cursorRow < screen.scrollTop || screen.cursorRow > screen.scrollBottom) return;
    count = std::max(1, std::min(count, screen.scrollBottom - screen.cursorRow + 1));
    for (int iteration = 0; iteration < count; ++iteration) {
        screen.lines.erase(screen.lines.begin() + screen.scrollBottom);
        screen.lines.insert(screen.lines.begin() + screen.cursorRow, BlankLine());
    }
}

void TerminalModel::DeleteLines(int count) {
    Screen& screen = ActiveScreen();
    if (screen.cursorRow < screen.scrollTop || screen.cursorRow > screen.scrollBottom) return;
    count = std::max(1, std::min(count, screen.scrollBottom - screen.cursorRow + 1));
    for (int iteration = 0; iteration < count; ++iteration) {
        screen.lines.erase(screen.lines.begin() + screen.cursorRow);
        screen.lines.insert(screen.lines.begin() + screen.scrollBottom, BlankLine());
    }
}

void TerminalModel::ExecuteEscape(wchar_t character, TerminalFeedResult& result) {
    Screen& screen = ActiveScreen();
    switch (character) {
    case L'7':
        screen.savedRow = screen.cursorRow;
        screen.savedColumn = screen.cursorColumn;
        screen.savedAttributes = attributes_;
        break;
    case L'8':
        screen.cursorRow = std::max(0, std::min(rows_ - 1, screen.savedRow));
        screen.cursorColumn = std::max(0, std::min(columns_ - 1, screen.savedColumn));
        attributes_ = screen.savedAttributes;
        break;
    case L'D': LineFeed(result); break;
    case L'M': ReverseIndex(); break;
    case L'E':
        screen.cursorColumn = 0;
        LineFeed(result);
        break;
    case L'c': Reset(); break;
    case L'=':
    case L'>':
    default:
        break;
    }
}

std::vector<int> TerminalModel::ParseParameters(std::wstring text, wchar_t& prefix) const {
    prefix = 0;
    if (!text.empty() && (text.front() == L'?' || text.front() == L'>' || text.front() == L'!' ||
                         text.front() == L'<' || text.front() == L'=')) {
        prefix = text.front();
        text.erase(text.begin());
    }
    while (!text.empty() && text.back() >= 0x20 && text.back() <= 0x2f) text.pop_back();
    std::vector<int> values;
    size_t start = 0;
    do {
        const size_t separator = text.find(L';', start);
        const std::wstring part = text.substr(start,
            separator == std::wstring::npos ? std::wstring::npos : separator - start);
        if (part.empty()) values.push_back(0);
        else {
            int parsed = 0;
            for (wchar_t digit : part) {
                if (digit < L'0' || digit > L'9') return {};
                // Saturate before arithmetic; terminal coordinates never need
                // an unbounded signed value from an untrusted peer.
                parsed = std::min(65535, parsed * 10 + (digit - L'0'));
            }
            values.push_back(parsed);
        }
        if (separator == std::wstring::npos) break;
        start = separator + 1;
    } while (start <= text.size());
    if (values.empty()) values.push_back(0);
    return values;
}

int TerminalModel::Parameter(const std::vector<int>& parameters, size_t index, int fallback) const {
    if (index >= parameters.size() || parameters[index] == 0) return fallback;
    return parameters[index];
}

void TerminalModel::ExecuteCsi(wchar_t finalCharacter, TerminalFeedResult& result) {
    wchar_t prefix = 0;
    const std::vector<int> parameters = ParseParameters(csiBuffer_, prefix);
    if (parameters.empty()) return;
    Screen& screen = ActiveScreen();
    const int amount = Parameter(parameters, 0, 1);
    switch (finalCharacter) {
    case L'A': screen.cursorRow = std::max(originMode_ ? screen.scrollTop : 0, screen.cursorRow - amount); break;
    case L'B': screen.cursorRow = std::min(originMode_ ? screen.scrollBottom : rows_ - 1, screen.cursorRow + amount); break;
    case L'C': screen.cursorColumn = std::min(columns_ - 1, screen.cursorColumn + amount); break;
    case L'D': screen.cursorColumn = std::max(0, screen.cursorColumn - amount); break;
    case L'E':
        screen.cursorRow = std::min(screen.scrollBottom, screen.cursorRow + amount);
        screen.cursorColumn = 0;
        break;
    case L'F':
        screen.cursorRow = std::max(screen.scrollTop, screen.cursorRow - amount);
        screen.cursorColumn = 0;
        break;
    case L'G':
    case L'`': screen.cursorColumn = std::max(0, std::min(columns_ - 1, amount - 1)); break;
    case L'H':
    case L'f':
        screen.cursorRow = std::max(originMode_ ? screen.scrollTop : 0,
            std::min(originMode_ ? screen.scrollBottom : rows_ - 1,
                Parameter(parameters, 0, 1) - 1 + (originMode_ ? screen.scrollTop : 0)));
        screen.cursorColumn = std::max(0, std::min(columns_ - 1, Parameter(parameters, 1, 1) - 1));
        break;
    case L'd': screen.cursorRow = std::max(0, std::min(rows_ - 1, amount - 1)); break;
    case L'J': EraseDisplay(parameters[0]); break;
    case L'K': EraseLine(parameters[0]); break;
    case L'X': EraseCharacters(amount); break;
    case L'@': InsertCharacters(amount); break;
    case L'P': DeleteCharacters(amount); break;
    case L'L': InsertLines(amount); break;
    case L'M': DeleteLines(amount); break;
    case L'S': ScrollUp(amount, &result); break;
    case L'T': ScrollDown(amount); break;
    case L'm': SetGraphicsRendition(parameters); break;
    case L'b':
        if (lastPrintedCharacter_ != 0) {
            for (int repeat = 0; repeat < std::min(amount, 4096); ++repeat)
                PutCharacter(lastPrintedCharacter_, L"", result);
        }
        break;
    case L's':
        if (prefix) break;
        screen.savedRow = screen.cursorRow;
        screen.savedColumn = screen.cursorColumn;
        screen.savedAttributes = attributes_;
        break;
    case L'u':
        // Kitty keyboard negotiation shares its final byte with legacy SCORC.
        // Queries and mode changes must never move the cursor or clear wrap.
        if (prefix == L'?') {
            if (responseRole_ == ResponseRole::Owner)
                result.response += "\x1b[?" + std::to_string(screen.keyboardFlags) + "u";
            return;
        }
        if (prefix == L'>' || prefix == L'=') {
            if (prefix == L'>' && screen.keyboardStack.size() < 16)
                screen.keyboardStack.push_back(screen.keyboardFlags);
            const unsigned flags = static_cast<unsigned>(parameters[0]) & 1u;
            const int mode = Parameter(parameters, 1, 1);
            if (mode == 1) screen.keyboardFlags = flags;
            else if (mode == 2) screen.keyboardFlags |= flags;
            else if (mode == 3) screen.keyboardFlags &= ~flags;
            return;
        }
        if (prefix == L'<') {
            for (int count = std::min(amount, 16); count > 0; --count) {
                screen.keyboardFlags = screen.keyboardStack.empty() ? 0 : screen.keyboardStack.back();
                if (!screen.keyboardStack.empty()) screen.keyboardStack.pop_back();
            }
            return;
        }
        if (prefix) return;
        screen.cursorRow = std::max(0, std::min(rows_ - 1, screen.savedRow));
        screen.cursorColumn = std::max(0, std::min(columns_ - 1, screen.savedColumn));
        attributes_ = screen.savedAttributes;
        break;
    case L'r': {
        const int top = Parameter(parameters, 0, 1) - 1;
        const int bottom = Parameter(parameters, 1, rows_) - 1;
        if (top >= 0 && bottom < rows_ && top < bottom) {
            screen.scrollTop = top;
            screen.scrollBottom = bottom;
            screen.cursorRow = 0;
            screen.cursorColumn = 0;
        }
        break;
    }
    case L'h':
    case L'l':
        if (prefix == L'?') {
            for (const int mode : parameters) SetPrivateMode(mode, finalCharacter == L'h');
        } else {
            for (const int mode : parameters) {
                if (mode == 4) insertMode_ = finalCharacter == L'h';
            }
        }
        break;
    case L'n':
        if (responseRole_ != ResponseRole::Owner) break;
        if (parameters[0] == 5) result.response += "\x1b[0n";
        else if (parameters[0] == 6) {
            result.response += "\x1b[" + std::to_string(screen.cursorRow + 1) + ";" +
                std::to_string(screen.cursorColumn + 1) + "R";
        }
        break;
    case L'p':
        if (prefix == L'?' && !csiBuffer_.empty() && csiBuffer_.back() == L'$' &&
            responseRole_ == ResponseRole::Owner) {
            const int mode = parameters[0];
            const bool known = mode == 1 || mode == 7 || mode == 25 || mode == 1000 || mode == 1002 ||
                mode == 1003 || mode == 1006 || mode == 1007 || mode == 1049 || mode == 2004 || mode == 2026;
            const bool enabled = mode == 1 ? applicationCursorKeys_ : mode == 7 ? autoWrap_ :
                mode == 25 ? cursorVisible_ : mode == 1006 ? sgrMouse_ : mode == 1007 ? alternateScroll_ :
                mode == 1049 ? alternateScreen_ : mode == 2004 ? bracketedPaste_ : mode == mouseTracking_;
            result.response += "\x1b[?" + std::to_string(mode) + ";" +
                std::to_string(known ? (enabled ? 1 : 2) : 0) + "$y";
            return;
        }
        break;
    case L'c':
        if (responseRole_ != ResponseRole::Owner) break;
        result.response += prefix == L'>' ? "\x1b[>0;136;0c" : "\x1b[?1;2c";
        break;
    case L't':
        if (responseRole_ != ResponseRole::Owner) break;
        if (parameters[0] == 18 || parameters[0] == 19)
            result.response += "\x1b[8;" + std::to_string(rows_) + ";" +
                std::to_string(columns_) + "t";
        break;
    default:
        break;
    }
    if (finalCharacter != L'm')
        screen.wrapPending = false;
}

void TerminalModel::SetGraphicsRendition(const std::vector<int>& parameters) {
    for (size_t index = 0; index < parameters.size(); ++index) {
        const int parameter = parameters[index];
        if (parameter == 0) attributes_ = {};
        else if (parameter == 1) attributes_.bold = true;
        else if (parameter == 4) attributes_.underline = true;
        else if (parameter == 7) attributes_.inverse = true;
        else if (parameter == 22) attributes_.bold = false;
        else if (parameter == 24) attributes_.underline = false;
        else if (parameter == 27) attributes_.inverse = false;
        else if (parameter >= 30 && parameter <= 37) attributes_.foreground = IndexedColor(parameter - 30);
        else if (parameter == 39) attributes_.foreground = {};
        else if (parameter >= 40 && parameter <= 47) attributes_.background = IndexedColor(parameter - 40);
        else if (parameter == 49) attributes_.background = {};
        else if (parameter >= 90 && parameter <= 97) attributes_.foreground = IndexedColor(parameter - 90 + 8);
        else if (parameter >= 100 && parameter <= 107) attributes_.background = IndexedColor(parameter - 100 + 8);
        else if ((parameter == 38 || parameter == 48) && index + 1 < parameters.size()) {
            TerminalColor color;
            if (parameters[index + 1] == 5 && index + 2 < parameters.size()) {
                color = IndexedColor(parameters[index + 2]);
                index += 2;
            } else if (parameters[index + 1] == 2 && index + 4 < parameters.size()) {
                color = RgbColor(parameters[index + 2], parameters[index + 3], parameters[index + 4]);
                index += 4;
            } else {
                continue;
            }
            if (parameter == 38) attributes_.foreground = color;
            else attributes_.background = color;
        }
    }
}

void TerminalModel::SetPrivateMode(int mode, bool enabled) {
    switch (mode) {
    case 1: applicationCursorKeys_ = enabled; break;
    case 6: originMode_ = enabled; break;
    case 7: autoWrap_ = enabled; break;
    case 25: cursorVisible_ = enabled; break;
    case 47:
    case 1047: SwitchAlternateScreen(enabled, false); break;
    case 1048: {
        Screen& screen = ActiveScreen();
        if (enabled) {
            screen.savedRow = screen.cursorRow;
            screen.savedColumn = screen.cursorColumn;
            screen.savedAttributes = attributes_;
        } else {
            screen.cursorRow = screen.savedRow;
            screen.cursorColumn = screen.savedColumn;
            attributes_ = screen.savedAttributes;
        }
        break;
    }
    case 1049: SwitchAlternateScreen(enabled, true); break;
    case 1000:
    case 1002:
    case 1003:
        if (enabled) mouseTracking_ = mode;
        else if (mouseTracking_ == mode) mouseTracking_ = 0;
        break;
    case 1006: sgrMouse_ = enabled; break;
    case 1007: alternateScroll_ = enabled; break;
    case 2004: bracketedPaste_ = enabled; break;
    default: break;
    }
}

void TerminalModel::SwitchAlternateScreen(bool enabled, bool saveCursor) {
    if (enabled == alternateScreen_) return;
    if (enabled) {
        primaryModes_ = (autoWrap_ ? 1u : 0u) | (originMode_ ? 2u : 0u) | (insertMode_ ? 4u : 0u) |
            (cursorVisible_ ? 8u : 0u) | (applicationCursorKeys_ ? 16u : 0u) |
            (bracketedPaste_ ? 32u : 0u) | (sgrMouse_ ? 64u : 0u) | (alternateScroll_ ? 128u : 0u) |
            (static_cast<unsigned>(mouseTracking_) << 8);
        if (saveCursor) {
            primary_.savedRow = primary_.cursorRow;
            primary_.savedColumn = primary_.cursorColumn;
            primary_.savedAttributes = attributes_;
        }
        alternate_ = BlankScreen();
        alternateScreen_ = true;
    } else {
        alternateScreen_ = false;
        autoWrap_ = (primaryModes_ & 1) != 0;
        originMode_ = (primaryModes_ & 2) != 0;
        insertMode_ = (primaryModes_ & 4) != 0;
        cursorVisible_ = (primaryModes_ & 8) != 0;
        applicationCursorKeys_ = (primaryModes_ & 16) != 0;
        bracketedPaste_ = (primaryModes_ & 32) != 0;
        sgrMouse_ = (primaryModes_ & 64) != 0;
        alternateScroll_ = (primaryModes_ & 128) != 0;
        mouseTracking_ = static_cast<int>(primaryModes_ >> 8);
        if (saveCursor) {
            primary_.cursorRow = std::max(0, std::min(rows_ - 1, primary_.savedRow));
            primary_.cursorColumn = std::max(0, std::min(columns_ - 1, primary_.savedColumn));
            attributes_ = primary_.savedAttributes;
        }
    }
}

std::wstring TerminalModel::ScreenText(size_t maximumRows) const {
    std::wstring text;
    const auto &lines = ActiveScreen().lines;
    for (size_t row = 0; row < lines.size() && row < maximumRows; ++row) {
        const auto &line = lines[row];
        std::wstring value;
        for (size_t col = 0; col < line.used && col < line.cells.size(); ++col)
            if (!line.cells[col].continuation) value += line.cells[col].character;
        while (!value.empty() && value.back() == L' ') value.pop_back();
        text += value;
        text += L'\n';
    }
    return text;
}

std::string TerminalModel::KeySequence(std::string name, unsigned modifiers) const {
    if (name.empty() || name.size() > 64 || modifiers > 7) return {};
    for (;;) {
        if (name.rfind("Shift+", 0) == 0) { modifiers |= 1; name.erase(0, 6); }
        else if (name.rfind("Alt+", 0) == 0) { modifiers |= 2; name.erase(0, 4); }
        else if (name.rfind("Ctrl+", 0) == 0) { modifiers |= 4; name.erase(0, 5); }
        else break;
    }
    const int code = name == "Enter" ? 13 : name == "Tab" ? 9 : name == "Escape" || name == "Esc" ? 27 :
        name == "Backspace" ? 127 : name == "Space" ? 32 : name.size() == 1 ? static_cast<unsigned char>(name[0]) : 0;
    const int parameter = 1 + static_cast<int>(modifiers);
    if (code) {
        if (code < 32 && name.size() == 1) return {};
        if (KeyboardFlags() && (code == 13 || code == 9 || code == 27 || code == 127 || modifiers)) {
            int key = code;
            if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
            return "\x1b[" + std::to_string(key) + (modifiers ? ";" + std::to_string(parameter) : "") + "u";
        }
        if (name == "Tab" && modifiers == 1) return "\x1b[Z";
        char value = static_cast<char>(code);
        if (modifiers & 4) {
            if (value == ' ' || value == '@') value = 0;
            else if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= '_')) value &= 31;
            else if (value == '?') value = 127;
            else return {};
        }
        return (modifiers & 2 ? std::string("\x1b") : std::string()) + std::string(1, value);
    }
    char final = name == "Up" ? 'A' : name == "Down" ? 'B' : name == "Right" ? 'C' : name == "Left" ? 'D' :
        name == "Home" ? 'H' : name == "End" ? 'F' : 0;
    if (final) return modifiers ? "\x1b[1;" + std::to_string(parameter) + final :
        std::string(applicationCursorKeys_ ? "\x1bO" : "\x1b[") + final;
    if (name == "F1" || name == "F2" || name == "F3" || name == "F4") {
        final = static_cast<char>('P' + name[1] - '1');
        return modifiers ? "\x1b[1;" + std::to_string(parameter) + final : std::string("\x1bO") + final;
    }
    const int number = name == "Insert" ? 2 : name == "Delete" ? 3 : name == "PageUp" ? 5 : name == "PageDown" ? 6 :
        name == "F5" ? 15 : name == "F6" ? 17 : name == "F7" ? 18 : name == "F8" ? 19 :
        name == "F9" ? 20 : name == "F10" ? 21 : name == "F11" ? 23 : name == "F12" ? 24 : 0;
    return number ? "\x1b[" + std::to_string(number) + (modifiers ? ";" + std::to_string(parameter) : "") + "~" : "";
}

std::string TerminalModel::MouseSequence(int button, int column, int row, unsigned modifiers, bool release) const {
    if (!mouseTracking_ || modifiers > 7 || column < 0 || row < 0 || column >= columns_ || row >= rows_) return {};
    const int flags = button | ((modifiers & 1) ? 4 : 0) | ((modifiers & 2) ? 8 : 0) | ((modifiers & 4) ? 16 : 0);
    if (sgrMouse_)
        return "\x1b[<" + std::to_string(flags) + ";" + std::to_string(column + 1) + ";" +
            std::to_string(row + 1) + (release ? "m" : "M");
    if (column >= 223 || row >= 223) return {};
    return std::string("\x1b[M") + static_cast<char>(32 + (release ? 3 : flags)) +
        static_cast<char>(33 + column) + static_cast<char>(33 + row);
}

std::string TerminalModel::WheelSequence(bool up, int column, int row, unsigned modifiers) const {
    if (mouseTracking_) return MouseSequence(up ? 64 : 65, column, row, modifiers);
    return alternateScreen_ && alternateScroll_ ? KeySequence(up ? "Up" : "Down", modifiers) : "";
}

void TerminalModel::AppendLogCharacter(
    wchar_t character, const std::wstring& timestamp, TerminalFeedResult& result) {
    if (character == L'\r') return;
    if (logAtLineStart_ && !timestamp.empty())
        result.logText += L"[" + timestamp + L"] ";
    result.logText += character;
    logAtLineStart_ = character == L'\n';
}

void TerminalModel::EnsureTimestamp(TerminalLine& line, const std::wstring& timestamp) {
    if (line.timestamp.empty() && !timestamp.empty()) line.timestamp = timestamp;
}

void TerminalModel::Resize(int columns, int rows) {
    ++revision_;
    projectionDirty_ = true;
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    if (columns == columns_ && rows == rows_)
        return;
    const int oldColumns = columns_;
    const int oldRows = rows_;
    columns_ = columns;
    rows_ = rows;
    ResizeScreen(primary_, true, oldColumns, oldRows);
    ResizeScreen(alternate_, false, oldColumns, oldRows);
}

void TerminalModel::ResizeScreen(Screen& screen, bool preserveHistory, int oldColumns, int oldRows) {
    (void)oldRows;
    if (!preserveHistory) {
        // Alternate screen is a VT grid, never a log to reflow.
        screen.lines.resize(static_cast<size_t>(rows_), BlankLine());
        for (auto &line : screen.lines) {
            line.cells.resize(static_cast<size_t>(columns_));
            line.used = std::min(line.used, line.cells.size());
        }
        screen.cursorRow = std::clamp(screen.cursorRow, 0, rows_ - 1);
        screen.cursorColumn = std::clamp(screen.cursorColumn, 0, columns_ - 1);
        screen.savedRow = std::clamp(screen.savedRow, 0, rows_ - 1);
        screen.savedColumn = std::clamp(screen.savedColumn, 0, columns_ - 1);
        screen.scrollTop = 0;
        screen.scrollBottom = rows_ - 1;
        screen.wrapPending = false;
        return;
    }
    std::vector<TerminalLine> old;
    old.reserve(history_.size() + screen.lines.size());
    for (const auto& line : history_) old.push_back(*line);
    const auto cursorRows = static_cast<size_t>(std::max(screen.cursorRow, screen.savedRow) + 1);
    size_t meaningful = cursorRows;
    for (size_t i = 0; i < screen.lines.size(); ++i)
        if (screen.lines[i].used || screen.lines[i].hardBreak || screen.lines[i].wrapped ||
            !screen.lines[i].timestamp.empty())
            meaningful = i + 1;
    meaningful = std::max(meaningful, cursorRows);
    old.insert(old.end(), screen.lines.begin(), screen.lines.begin() + std::min(meaningful, screen.lines.size()));
    size_t cursorOld = history_.size() + static_cast<size_t>(screen.cursorRow);
    size_t savedOld = history_.size() + static_cast<size_t>(screen.savedRow);
    const int cursorPosition = screen.cursorColumn + (screen.wrapPending ? 1 : 0);
    struct Group {
        std::vector<TerminalCell> cells;
        std::wstring stamp;
        bool hard = false;
        std::uint64_t id = 0;
        size_t base = 0;
    };
    std::vector<Group> groups;
    size_t cursorGroup = 0, cursorOffset = 0, savedGroup = 0, savedOffset = 0;
    for (size_t i = 0; i < old.size(); ++i) {
        auto &line = old[i];
        if (i == 0 || !old[i - 1].wrapped)
            groups.push_back({{}, line.timestamp, line.hardBreak, line.logicalId, line.logicalOffset});
        auto &g = groups.back();
        if (i == cursorOld) {
            cursorGroup = groups.size() - 1;
            cursorOffset = g.cells.size() + static_cast<size_t>(cursorPosition);
        }
        if (i == savedOld) {
            savedGroup = groups.size() - 1;
            savedOffset = g.cells.size() + static_cast<size_t>(screen.savedColumn);
        }
        size_t count = std::min(line.used, line.cells.size());
        if (i == cursorOld)
            count = std::max(count, std::min(line.cells.size(), static_cast<size_t>(cursorPosition)));
        // Attributes and explicit spaces are data; unused grid padding is not.
        g.cells.insert(g.cells.end(), line.cells.begin(), line.cells.begin() + count);
        g.hard = line.hardBreak;
    }
    std::vector<TerminalLine> lines;
    size_t cursorAbsolute = 0, savedAbsolute = 0;
    int cursorColumn = 0, savedColumn = 0;
    bool pending = false;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        auto &g = groups[gi];
        size_t at = 0;
        bool first = true;
        do {
            size_t take = std::min(static_cast<size_t>(columns_), g.cells.size() - at);
            if (take && at + take < g.cells.size() && g.cells[at + take].continuation)
                --take;
            if (take == 0 && at < g.cells.size())
                take = std::min<size_t>(2, g.cells.size() - at);
            auto line = BlankLine();
            line.logicalId = g.id;
            line.logicalOffset = g.base + at;
            line.timestamp = first ? g.stamp : L"";
            line.used = take;
            if (line.cells.size() < take)
                line.cells.resize(take);
            std::copy_n(g.cells.begin() + at, take, line.cells.begin());
            line.wrapped = at + take < g.cells.size();
            line.hardBreak = !line.wrapped && g.hard;
            auto map = [&](size_t offset, size_t &row, int &col, bool cursor) {
                if (offset >= at && (offset < at + take || !line.wrapped)) {
                    row = lines.size();
                    size_t local = offset - at;
                    col = static_cast<int>(std::min(local, static_cast<size_t>(columns_ - 1)));
                    if (cursor)
                        pending = local >= static_cast<size_t>(columns_) && autoWrap_;
                }
            };
            if (gi == cursorGroup)
                map(cursorOffset, cursorAbsolute, cursorColumn, true);
            if (gi == savedGroup)
                map(savedOffset, savedAbsolute, savedColumn, false);
            lines.push_back(std::move(line));
            at += take;
            first = false;
        } while (at < g.cells.size());
    }
    if (lines.empty())
        lines.push_back(BlankLine());
    const size_t start = lines.size() > static_cast<size_t>(rows_) ? lines.size() - static_cast<size_t>(rows_) : 0;
    history_.clear();
    for (size_t i = 0; i < start; ++i)
        history_.push_back(std::make_shared<const TerminalLine>(std::move(lines[i])));
    // Reflow changes visual row count, not the retained device output. Only
    // newly received output may evict old content under the stable budget.
    RecountHistory();
    screen.lines.assign(lines.begin() + start, lines.end());
    while (screen.lines.size() < static_cast<size_t>(rows_))
        screen.lines.push_back(BlankLine());
    screen.cursorRow = static_cast<int>(cursorAbsolute >= start ? cursorAbsolute - start : 0);
    screen.cursorColumn = cursorColumn;
    screen.savedRow = static_cast<int>(savedAbsolute >= start ? savedAbsolute - start : 0);
    screen.savedColumn = savedColumn;
    screen.cursorRow = std::clamp(screen.cursorRow, 0, rows_ - 1);
    screen.savedRow = std::clamp(screen.savedRow, 0, rows_ - 1);
    screen.scrollTop = 0;
    screen.scrollBottom = rows_ - 1;
    screen.wrapPending = pending;
    resizeHistoryLines_ = 0;
    (void)oldColumns;
}

TerminalModel::Anchor TerminalModel::CaptureAnchor(size_t line, int column) const {
    const auto &value = DisplayLine(line);
    return {value.logicalId, value.logicalOffset + static_cast<size_t>(std::max(0, column))};
}
std::pair<size_t, int> TerminalModel::LocateAnchor(Anchor anchor) const {
    size_t best = 0;
    int col = 0;
    for (size_t i = 0; i < DisplayLineCount(); ++i) {
        const auto &line = DisplayLine(i);
        if (line.logicalId == anchor.id && anchor.offset >= line.logicalOffset) {
            best = i;
            col = static_cast<int>(std::min(anchor.offset - line.logicalOffset, line.cells.size() - 1));
            if (anchor.offset < line.logicalOffset + line.used)
                break;
        }
    }
    return {best, col};
}
TerminalModel TerminalModel::PreviewInput(const std::wstring &text, size_t cursor) const {
    if (!lineWrapView_ && !alternateScreen_) {
        TerminalModel model = *this;
        model.SetLineWrapView(true);
        cursor = std::min(cursor, text.size());
        model.Feed(text.substr(0, cursor), L"");
        const auto anchor = model.CaptureAnchor(model.CursorDisplayLine(), model.CursorColumn());
        const bool pending = model.primary_.wrapPending;
        model.Feed(text.substr(cursor), L"");
        const auto caret = model.LocateAnchor(anchor);
        model.primary_.cursorRow =
            static_cast<int>(caret.first >= model.HistorySize() ? caret.first - model.HistorySize() : 0);
        model.primary_.cursorColumn = caret.second;
        model.primary_.wrapPending = pending;
        model.SetLineWrapView(false);
        return model;
    }
    TerminalModel model(columns_, rows_);
    model.primary_ = primary_;
    model.attributes_ = attributes_;
    model.nextLogicalId_ = nextLogicalId_;
    // Copy only the live grid. The temporary draft may wrap into temporary history.
    cursor = std::min(cursor, text.size());
    model.Feed(text.substr(0, cursor), L"");
    const bool pending = model.primary_.wrapPending;
    auto anchor = model.CaptureAnchor(model.CursorDisplayLine(), model.CursorColumn());
    model.Feed(text.substr(cursor), L"");
    auto caret = model.LocateAnchor(anchor);
    // Home/Left in a draft longer than the screen must reveal the caret, rather
    // than restoring a stale grid row after suffix output scrolls it away.
    size_t start = std::min(model.HistorySize(), caret.first);
    std::vector<TerminalLine> view;
    for (size_t i = 0; i < static_cast<size_t>(rows_); ++i)
        view.push_back(start + i < model.DisplayLineCount() ? model.DisplayLine(start + i) : model.BlankLine());
    model.ClearHistory();
    model.primary_.lines = std::move(view);
    model.primary_.cursorRow = static_cast<int>(caret.first - start);
    model.primary_.cursorColumn = caret.second;
    model.primary_.wrapPending = pending;
    return model;
}

void TerminalModel::Reset() {
    ++revision_;
    projectionDirty_ = true;
    ClearHistory();
    resizeHistoryLines_ = 0;
    primary_ = BlankScreen();
    alternate_ = BlankScreen();
    alternateScreen_ = false;
    cursorVisible_ = true;
    applicationCursorKeys_ = false;
    bracketedPaste_ = false;
    mouseTracking_ = 0;
    sgrMouse_ = false;
    alternateScroll_ = true;
    primaryModes_ = 0;
    autoWrap_ = true;
    originMode_ = false;
    insertMode_ = false;
    decSpecialGraphics_ = false;
    escapeIntermediate_ = 0;
    lastPrintedCharacter_ = 0;
    attributes_ = {};
    savedAttributes_ = {};
    parserState_ = ParserState::Ground;
    csiBuffer_.clear();
    oscBuffer_.clear(); oscOverflow_ = false;
    logAtLineStart_ = true;
}

void TerminalModel::Clear() {
    ++revision_;
    projectionDirty_ = true;
    Screen& screen = ActiveScreen();
    for (TerminalLine& line : screen.lines) line = BlankLine();
    screen.cursorRow = 0;
    screen.cursorColumn = 0;
    screen.scrollTop = 0;
    screen.scrollBottom = rows_ - 1;
    screen.wrapPending = false;
    if (!alternateScreen_)
        ClearHistory();
}

void TerminalModel::ClearScrollback() {
    ++revision_;
    projectionDirty_ = true;
    ClearHistory();
    resizeHistoryLines_ = 0;
}

size_t TerminalModel::HistorySize() const {
    return alternateScreen_ ? 0 : history_.size();
}

void TerminalModel::SetLineWrapView(bool enabled) {
    ++revision_;
    if (lineWrapView_ != enabled) {
        lineWrapView_ = enabled;
        projectionDirty_ = true;
    }
}
const TerminalLine &TerminalModel::RawDisplayLine(size_t index) const {
    if (!alternateScreen_ && index < history_.size())
        return *history_[index];
    const size_t screenIndex = index - HistorySize();
    if (screenIndex < ActiveScreen().lines.size())
        return ActiveScreen().lines[screenIndex];
    return ActiveScreen().lines.back();
}
void TerminalModel::EnsureProjection() const {
    if (!projectionDirty_)
        return;
    // Completed history rows are immutable until trimming, clearing or resize.
    // Preserve their projections and revisit only the last historical logical
    // line plus the changed screen. A TUI repaint must not rebuild 20k rows.
    size_t keep = 0, start = 0;
    if (!alternateScreen_ && projectedHistoryRevision_ == historyRevision_ &&
        projectedHistorySize_ && projectedHistorySize_ <= HistorySize() && !projectionStarts_.empty()) {
        auto boundary = std::upper_bound(projectionStarts_.begin(), projectionStarts_.end(), projectedHistorySize_ - 1);
        if (boundary != projectionStarts_.begin()) {
            --boundary;
            keep = static_cast<size_t>(boundary - projectionStarts_.begin());
            start = *boundary;
        }
    }
    projectionStarts_.resize(keep);
    projectedLines_.resize(keep);
    projectionReady_.resize(keep);
    projectionMaximum_.resize(keep);
    const size_t count = HistorySize() + ActiveScreen().lines.size();
    for (size_t i = start; i < count; ++i) {
        const auto &line = RawDisplayLine(i);
        if (i == start || !RawDisplayLine(i - 1).wrapped || !line.logicalId ||
            line.logicalId != RawDisplayLine(i - 1).logicalId ||
            line.logicalOffset <= RawDisplayLine(i - 1).logicalOffset)
            projectionStarts_.push_back(i);
    }
    projectedLines_.resize(projectionStarts_.size());
    projectionReady_.resize(projectionStarts_.size(), false);
    for (size_t i = keep; i < projectionStarts_.size(); ++i) {
        const size_t end = i + 1 < projectionStarts_.size() ? projectionStarts_[i + 1] : count;
        const auto &first = RawDisplayLine(projectionStarts_[i]);
        const auto &last = RawDisplayLine(end - 1);
        const size_t width = last.logicalOffset - first.logicalOffset + last.used;
        projectionMaximum_.push_back(std::max(i ? projectionMaximum_.back() : size_t(0), width));
    }
    projectedHistoryRevision_ = historyRevision_;
    projectedHistorySize_ = HistorySize();
    projectionDirty_ = false;
}
size_t TerminalModel::DisplayLineCount() const {
    if (lineWrapView_ || alternateScreen_)
        return HistorySize() + ActiveScreen().lines.size();
    EnsureProjection();
    return projectionStarts_.size();
}
size_t TerminalModel::MaximumDisplayColumns() const {
    size_t maximum = static_cast<size_t>(columns_);
    if (!lineWrapView_ && !alternateScreen_) {
        EnsureProjection();
        if (!projectionMaximum_.empty()) maximum = std::max(maximum, projectionMaximum_.back());
    }
    // BlankLine, ResizeScreen and all VT edits keep every raw row at columns_.
    // Long logical rows exist only in the projection handled above.
    return maximum;
}
const TerminalLine& TerminalModel::DisplayLine(size_t index) const {
    if (lineWrapView_ || alternateScreen_)
        return RawDisplayLine(index);
    EnsureProjection();
    index = std::min(index, projectionStarts_.size() - 1);
    if (!projectionReady_[index]) {
        const auto start = projectionStarts_[index];
        const auto end =
            index + 1 < projectionStarts_.size() ? projectionStarts_[index + 1] : HistorySize() + primary_.lines.size();
        TerminalLine joined = RawDisplayLine(start);
        if (end > start + 1) {
            joined.cells.clear();
            joined.used = 0;
            for (size_t i = start; i < end; ++i) {
                const auto &line = RawDisplayLine(i);
                const auto offset = line.logicalOffset - joined.logicalOffset;
                const auto used = std::min(line.used, line.cells.size());
                joined.cells.resize(offset + used);
                std::copy_n(line.cells.begin(), used, joined.cells.begin() + offset);
                joined.used = offset + used;
                joined.hardBreak = line.hardBreak;
            }
        }
        joined.wrapped = false;
        projectedLines_[index] = std::make_shared<const TerminalLine>(std::move(joined));
        projectionReady_[index] = true;
    }
    return *projectedLines_[index];
}
size_t TerminalModel::CursorDisplayLine() const {
    const auto raw = HistorySize() + static_cast<size_t>(ActiveScreen().cursorRow);
    if (lineWrapView_ || alternateScreen_)
        return raw;
    EnsureProjection();
    return static_cast<size_t>(std::upper_bound(projectionStarts_.begin(), projectionStarts_.end(), raw) -
                               projectionStarts_.begin() - 1);
}
int TerminalModel::CursorColumn() const {
    if (lineWrapView_ || alternateScreen_)
        return ActiveScreen().cursorColumn;
    const auto raw = HistorySize() + static_cast<size_t>(ActiveScreen().cursorRow);
    return static_cast<int>(RawDisplayLine(raw).logicalOffset - DisplayLine(CursorDisplayLine()).logicalOffset) +
           ActiveScreen().cursorColumn;
}

std::wstring TerminalModel::LineText(size_t displayLine, bool trimRight) const {
    if (displayLine >= DisplayLineCount()) return {};
    const TerminalLine& line = DisplayLine(displayLine);
    std::wstring text;
    text.reserve(line.cells.size());
    for (const TerminalCell& cell : line.cells) {
        if (!cell.continuation) text += cell.character;
    }
    if (trimRight) {
        while (!text.empty() && text.back() == L' ') text.pop_back();
    }
    return text;
}

std::wstring TerminalModel::CurrentLineText() const {
    return LineText(CursorDisplayLine());
}

std::wstring TerminalModel::SelectedText(size_t firstLine, int firstColumn,
    size_t lastLine, int lastColumn) const {
    const auto count = DisplayLineCount();
    if (!count) return {};
    if (firstLine > lastLine || (firstLine == lastLine && firstColumn > lastColumn)) {
        std::swap(firstLine, lastLine);
        std::swap(firstColumn, lastColumn);
    }
    firstLine = std::min(firstLine, count - 1);
    lastLine = std::min(lastLine, count - 1);
    std::wstring text;
    for (size_t index = firstLine; index <= lastLine; ++index) {
        const auto &line = DisplayLine(index);
        const size_t used = std::min(line.used, line.cells.size());
        const size_t begin = index == firstLine ? std::min(used, static_cast<size_t>(std::max(0, firstColumn))) : 0;
        const size_t end = index == lastLine ? std::min(used, static_cast<size_t>(std::max(0, lastColumn)) + 1) : used;
        for (size_t column = begin; column < end; ++column)
            if (!line.cells[column].continuation) text += line.cells[column].character;
        if (index < lastLine && !line.wrapped) text += L"\r\n";
    }
    return text;
}

std::wstring TerminalModel::PlainText(bool includeTimestamps) const {
    std::wstring text;
    const size_t count = DisplayLineCount();
    for (size_t index = 0; index < count; ++index) {
        const TerminalLine& line = DisplayLine(index);
        const std::wstring content = LineText(index);
        if (includeTimestamps && !line.timestamp.empty()) text += line.timestamp + L" ";
        text += content;
        if (index + 1 < count && !line.wrapped) text += L"\r\n";
    }
    while (text.size() >= 2 && text.compare(text.size() - 2, 2, L"\r\n") == 0)
        text.resize(text.size() - 2);
    return text;
}

bool TerminalModel::IsWideCharacter(wchar_t character) {
    const unsigned value = static_cast<unsigned>(character);
    return value >= 0x1100 &&
        (value <= 0x115f || value == 0x2329 || value == 0x232a ||
            (value >= 0x2e80 && value <= 0xa4cf && value != 0x303f) ||
            (value >= 0xac00 && value <= 0xd7a3) ||
            (value >= 0xf900 && value <= 0xfaff) ||
            (value >= 0xfe10 && value <= 0xfe19) ||
            (value >= 0xfe30 && value <= 0xfe6f) ||
            (value >= 0xff00 && value <= 0xff60) ||
            (value >= 0xffe0 && value <= 0xffe6));
}

bool TerminalModel::IsCombiningCharacter(wchar_t character) {
    const unsigned value = static_cast<unsigned>(character);
    return (value >= 0x0300 && value <= 0x036f) ||
        (value >= 0x1ab0 && value <= 0x1aff) ||
        (value >= 0x1dc0 && value <= 0x1dff) ||
        (value >= 0x20d0 && value <= 0x20ff) ||
        (value >= 0xfe20 && value <= 0xfe2f);
}

wchar_t TerminalModel::MapSpecialGraphics(wchar_t character) const {
    if (!decSpecialGraphics_) return character;
    switch (character) {
    case L'`': return L'◆';
    case L'a': return L'▒';
    case L'f': return L'°';
    case L'g': return L'±';
    case L'j': return L'┘';
    case L'k': return L'┐';
    case L'l': return L'┌';
    case L'm': return L'└';
    case L'n': return L'┼';
    case L'o': return L'⎺';
    case L'p': return L'⎻';
    case L'q': return L'─';
    case L'r': return L'⎼';
    case L's': return L'⎽';
    case L't': return L'├';
    case L'u': return L'┤';
    case L'v': return L'┴';
    case L'w': return L'┬';
    case L'x': return L'│';
    case L'y': return L'≤';
    case L'z': return L'≥';
    case L'{': return L'π';
    case L'|': return L'≠';
    case L'}': return L'£';
    case L'~': return L'·';
    default: return character;
    }
}

} // namespace serialctl
