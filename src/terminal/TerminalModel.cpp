#include "TerminalModel.h"

#include <algorithm>
#include <cwctype>

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
    primary_ = BlankScreen();
    alternate_ = BlankScreen();
}

TerminalLine TerminalModel::BlankLine() const {
    TerminalLine line;
    line.cells.resize(static_cast<size_t>(columns_));
    return line;
}

TerminalModel::Screen TerminalModel::BlankScreen() const {
    Screen screen;
    screen.lines.assign(static_cast<size_t>(rows_), BlankLine());
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
    TerminalFeedResult result;
    for (const wchar_t character : text) {
        switch (parserState_) {
        case ParserState::Ground:
            if (character == 0x9b) {
                csiBuffer_.clear();
                parserState_ = ParserState::Csi;
            } else if (character == 0x9d) {
                oscBuffer_.clear();
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
                oscBuffer_.clear();
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
                oscBuffer_.clear();
                parserState_ = ParserState::Ground;
            } else if (character == 0x1b) {
                parserState_ = ParserState::OscEscape;
            } else if (oscBuffer_.size() < 1024) {
                oscBuffer_ += character;
            }
            break;
        case ParserState::OscEscape:
            if (character == L'\\') {
                HandleOsc(result);
                oscBuffer_.clear();
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
    return result;
}

void TerminalModel::PutCharacter(
    wchar_t character, const std::wstring& timestamp, TerminalFeedResult& result) {
    Screen& screen = ActiveScreen();
    character = MapSpecialGraphics(character);
    const int width = IsWideCharacter(character) ? 2 : 1;
    if (screen.wrapPending || screen.cursorColumn + width > columns_) {
        if (autoWrap_) {
            screen.lines[static_cast<size_t>(screen.cursorRow)].wrapped = true;
            screen.cursorColumn = 0;
            screen.wrapPending = false;
            LineFeed(result);
        } else {
            screen.cursorColumn = columns_ - 1;
            screen.wrapPending = false;
        }
    }

    TerminalLine& line = screen.lines[static_cast<size_t>(screen.cursorRow)];
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
            history_.push_back(screen.lines.front());
            if (history_.size() > maximumScrollback_) history_.pop_front();
            if (result) ++result->scrollbackAdded;
        }
        screen.lines.erase(screen.lines.begin() + screen.scrollTop);
        screen.lines.insert(screen.lines.begin() + screen.scrollBottom, BlankLine());
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
        if (mode == 3) history_.clear();
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
    if (mode == 2) {
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
    std::move_backward(line.cells.begin() + screen.cursorColumn,
        line.cells.end() - count, line.cells.end());
    std::fill(line.cells.begin() + screen.cursorColumn,
        line.cells.begin() + screen.cursorColumn + count, TerminalCell{});
}

void TerminalModel::DeleteCharacters(int count) {
    Screen& screen = ActiveScreen();
    TerminalLine& line = screen.lines[static_cast<size_t>(screen.cursorRow)];
    count = std::max(1, std::min(count, columns_ - screen.cursorColumn));
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
        savedAttributes_ = attributes_;
        break;
    case L'8':
        screen.cursorRow = std::max(0, std::min(rows_ - 1, screen.savedRow));
        screen.cursorColumn = std::max(0, std::min(columns_ - 1, screen.savedColumn));
        attributes_ = savedAttributes_;
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
    if (!text.empty() && (text.front() == L'?' || text.front() == L'>' || text.front() == L'!')) {
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
        screen.savedRow = screen.cursorRow;
        screen.savedColumn = screen.cursorColumn;
        savedAttributes_ = attributes_;
        break;
    case L'u':
        screen.cursorRow = std::max(0, std::min(rows_ - 1, screen.savedRow));
        screen.cursorColumn = std::max(0, std::min(columns_ - 1, screen.savedColumn));
        attributes_ = savedAttributes_;
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
        if (parameters[0] == 5) result.response += "\x1b[0n";
        else if (parameters[0] == 6) {
            result.response += "\x1b[" + std::to_string(screen.cursorRow + 1) + ";" +
                std::to_string(screen.cursorColumn + 1) + "R";
        }
        break;
    case L'c':
        result.response += prefix == L'>' ? "\x1b[>0;136;0c" : "\x1b[?1;2c";
        break;
    case L't':
        if (parameters[0] == 18 || parameters[0] == 19)
            result.response += "\x1b[8;" + std::to_string(rows_) + ";" +
                std::to_string(columns_) + "t";
        break;
    default:
        break;
    }
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
            savedAttributes_ = attributes_;
        } else {
            screen.cursorRow = screen.savedRow;
            screen.cursorColumn = screen.savedColumn;
            attributes_ = savedAttributes_;
        }
        break;
    }
    case 1049: SwitchAlternateScreen(enabled, true); break;
    case 2004: bracketedPaste_ = enabled; break;
    default: break;
    }
}

void TerminalModel::SwitchAlternateScreen(bool enabled, bool saveCursor) {
    if (enabled == alternateScreen_) return;
    if (enabled) {
        if (saveCursor) {
            primary_.savedRow = primary_.cursorRow;
            primary_.savedColumn = primary_.cursorColumn;
            savedAttributes_ = attributes_;
        }
        alternate_ = BlankScreen();
        alternateScreen_ = true;
    } else {
        alternateScreen_ = false;
        if (saveCursor) {
            primary_.cursorRow = std::max(0, std::min(rows_ - 1, primary_.savedRow));
            primary_.cursorColumn = std::max(0, std::min(columns_ - 1, primary_.savedColumn));
            attributes_ = savedAttributes_;
        }
    }
}

void TerminalModel::AppendLogCharacter(
    wchar_t character, const std::wstring& timestamp, TerminalFeedResult& result) {
    if (character == L'\r') return;
    if (logAtLineStart_ && character != L'\n' && !timestamp.empty())
        result.logText += L"[" + timestamp + L"] ";
    result.logText += character;
    logAtLineStart_ = character == L'\n';
}

void TerminalModel::EnsureTimestamp(TerminalLine& line, const std::wstring& timestamp) {
    if (line.timestamp.empty() && !timestamp.empty()) line.timestamp = timestamp;
}

void TerminalModel::Resize(int columns, int rows) {
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    if (columns == columns_ && rows == rows_) return;
    const int oldColumns = columns_;
    const int oldRows = rows_;
    columns_ = columns;
    rows_ = rows;
    ResizeScreen(primary_, true, oldColumns, oldRows);
    ResizeScreen(alternate_, false, oldColumns, oldRows);
}

void TerminalModel::ResizeScreen(Screen& screen, bool preserveHistory, int oldColumns, int oldRows) {
    (void)oldColumns;
    if (rows_ < oldRows) {
        const int remove = oldRows - rows_;
        for (int index = 0; index < remove && !screen.lines.empty(); ++index) {
            if (preserveHistory) {
                history_.push_back(screen.lines.front());
                if (history_.size() > maximumScrollback_) history_.pop_front();
            }
            screen.lines.erase(screen.lines.begin());
        }
        screen.cursorRow = std::max(0, screen.cursorRow - remove);
    } else {
        while (static_cast<int>(screen.lines.size()) < rows_) screen.lines.push_back(BlankLine());
    }
    while (static_cast<int>(screen.lines.size()) > rows_) screen.lines.pop_back();
    for (TerminalLine& line : screen.lines) line.cells.resize(static_cast<size_t>(columns_));
    screen.cursorRow = std::max(0, std::min(rows_ - 1, screen.cursorRow));
    screen.cursorColumn = std::max(0, std::min(columns_ - 1, screen.cursorColumn));
    screen.savedRow = std::max(0, std::min(rows_ - 1, screen.savedRow));
    screen.savedColumn = std::max(0, std::min(columns_ - 1, screen.savedColumn));
    screen.scrollTop = 0;
    screen.scrollBottom = rows_ - 1;
    screen.wrapPending = false;
}

void TerminalModel::Reset() {
    history_.clear();
    primary_ = BlankScreen();
    alternate_ = BlankScreen();
    alternateScreen_ = false;
    cursorVisible_ = true;
    applicationCursorKeys_ = false;
    bracketedPaste_ = false;
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
    oscBuffer_.clear();
    logAtLineStart_ = true;
}

void TerminalModel::Clear() {
    Screen& screen = ActiveScreen();
    for (TerminalLine& line : screen.lines) line = BlankLine();
    screen.cursorRow = 0;
    screen.cursorColumn = 0;
    screen.scrollTop = 0;
    screen.scrollBottom = rows_ - 1;
    screen.wrapPending = false;
    if (!alternateScreen_) history_.clear();
}

void TerminalModel::ClearScrollback() {
    history_.clear();
}

size_t TerminalModel::HistorySize() const {
    return alternateScreen_ ? 0 : history_.size();
}

size_t TerminalModel::DisplayLineCount() const {
    return HistorySize() + ActiveScreen().lines.size();
}

size_t TerminalModel::MaximumDisplayColumns() const {
    size_t maximum = static_cast<size_t>(columns_);
    const size_t count = DisplayLineCount();
    for (size_t index = 0; index < count; ++index)
        maximum = std::max(maximum, DisplayLine(index).cells.size());
    return maximum;
}

const TerminalLine& TerminalModel::DisplayLine(size_t index) const {
    if (!alternateScreen_ && index < history_.size()) return history_[index];
    const size_t screenIndex = index - HistorySize();
    if (screenIndex < ActiveScreen().lines.size()) return ActiveScreen().lines[screenIndex];
    return ActiveScreen().lines.back();
}

size_t TerminalModel::CursorDisplayLine() const {
    return HistorySize() + static_cast<size_t>(ActiveScreen().cursorRow);
}

int TerminalModel::CursorColumn() const {
    return ActiveScreen().cursorColumn;
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
