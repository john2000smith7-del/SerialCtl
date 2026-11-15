#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace serialctl::ai::markup {

enum class Ink {
    Plain,
    Bold,
    Italic,
    BoldItalic,
    InlineCode,
    Keyword,
    String,
    Number,
    Comment,
    Name,
    Link,
    Strike,
    Underline,
    Mark,
    Kbd,
    Sub,
    Sup,
    Math,
    Muted
};

enum StyleFlag : unsigned {
    BoldFlag = 1u << 0,
    ItalicFlag = 1u << 1,
    StrikeFlag = 1u << 2,
    UnderlineFlag = 1u << 3,
    MarkFlag = 1u << 4,
    KbdFlag = 1u << 5,
    LinkFlag = 1u << 6,
    SubFlag = 1u << 7,
    SupFlag = 1u << 8,
    InlineCodeFlag = 1u << 9,
    MathFlag = 1u << 10,
    MutedFlag = 1u << 11
};

struct Span {
    size_t start = 0, length = 0;
    Ink ink = Ink::Plain;
    unsigned flags = 0;
};

struct LinkRange {
    size_t start = 0, length = 0;
    std::wstring url;
};

struct Text {
    std::wstring value;
    std::vector<Span> spans;
    std::vector<LinkRange> links;
};

enum class Kind { Prose, Heading, List, Quote, Rule, Code, Table, Details, Math };
enum class Alignment { Left, Center, Right };
enum class AlertKind { None, Note, Tip, Important, Warning, Caution };

struct LineMeta {
    int indent = 0;
    int quoteDepth = 0;
    int headingLevel = 0;
    size_t markerChars = 0;
    bool continuation = false;
    bool isTask = false;
    bool checked = false;
    bool isList = false;
};

struct Block {
    Kind kind = Kind::Prose;
    size_t source = 0, codeBegin = 0, codeEnd = 0;
    int headingLevel = 0;
    AlertKind alert = AlertKind::None;
    bool detailsOpen = false;
    std::wstring language;
    Text summary;
    std::vector<Text> lines;
    std::vector<LineMeta> lineMeta;
    std::vector<std::vector<Text>> rows;
    std::vector<Alignment> alignment;
    std::vector<Block> children;
};

std::vector<Block> Parse(std::wstring_view source, bool proseMarkup = true, bool inlineFormatted = false);
std::vector<Text> SplitTextLines(const Text &input);

} // namespace serialctl::ai::markup
