#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace serialctl::ai::markup {

enum class Ink { Plain, Bold, InlineCode, Keyword, String, Number, Comment, Name };
struct Span {
    size_t start, length;
    Ink ink;
};
struct Text {
    std::wstring value;
    std::vector<Span> spans;
};
enum class Kind { Prose, Heading, List, Rule, Code, Table };
enum class Alignment { Left, Center, Right };
struct Block {
    Kind kind = Kind::Prose;
    size_t source = 0, codeBegin = 0, codeEnd = 0;
    std::wstring language;
    std::vector<Text> lines;
    std::vector<std::vector<Text>> rows;
    std::vector<Alignment> alignment;
};

std::vector<Block> Parse(std::wstring_view source, bool proseMarkup = true);

} // namespace serialctl::ai::markup
