#include "TerminalDecoder.h"
#include <iostream>
int main() {
    const auto lead = [](std::uint8_t b) { return b >= 0x81 && b <= 0xfe; };
    using serialctl::IncompleteDbcsTail;
    if (IncompleteDbcsTail({0xd6, 0xd0}, lead) != 0 ||
        IncompleteDbcsTail({0xd6}, lead) != 1 ||
        IncompleteDbcsTail({0xd6, 0xd0, 0xce}, lead) != 1 ||
        IncompleteDbcsTail({0xd6, 0xd0, 'A'}, lead) != 0 ||
        IncompleteDbcsTail({'A', 0xd6, 0xd0, 0xce, 0xc4}, lead) != 0 ||
        IncompleteDbcsTail({}, lead) != 0) return 1;
    std::cout << "Terminal decoder tests passed\n";
}
