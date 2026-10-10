#include "TerminalDecoder.h"
#include <iostream>
int main() {
    const auto lead = [](std::uint8_t b) { return b >= 0x81 && b <= 0xfe; };
    using serialctl::IncompleteDbcsTail;
    if (IncompleteDbcsTail({0xd6, 0xd0}, lead) != 0 || IncompleteDbcsTail({0xd6}, lead) != 1 ||
        IncompleteDbcsTail({0xd6, 0xd0, 0xce}, lead) != 1 || IncompleteDbcsTail({0xd6, 0xd0, 'A'}, lead) != 0 ||
        IncompleteDbcsTail({'A', 0xd6, 0xd0, 0xce, 0xc4}, lead) != 0 || IncompleteDbcsTail({}, lead) != 0)
        return 1;
    using serialctl::IncompleteUtf8Tail;
    if (IncompleteUtf8Tail({0xe4, 0xb8}) != 2 || IncompleteUtf8Tail({0xe4, 0xb8, 0xad}) != 0 ||
        IncompleteUtf8Tail({0xff}) != 0 || IncompleteUtf8Tail({0xc2, 'A'}) != 0)
        return 1;
#ifdef _WIN32
    for (auto page : {UINT(CP_UTF8), UINT(936), UINT(437)}) {
        std::wstring expected = page == 437 ? L"abc\r\n" : L"A中文B\r\n";
        auto encoded = serialctl::WideToMultiByte(expected, page);
        for (size_t split = 0; split <= encoded.size(); ++split) {
            serialctl::TerminalDecoder decoder;
            std::vector<std::uint8_t> a(encoded.begin(), encoded.begin() + split),
                b(encoded.begin() + split, encoded.end());
            auto actual = decoder.Decode(a, page);
            actual += decoder.Decode(b, page);
            if (actual != expected)
                return 2;
        }
        serialctl::TerminalDecoder decoder;
        std::wstring actual;
        for (auto byte : encoded)
            actual += decoder.Decode({std::uint8_t(byte)}, page);
        if (actual != expected)
            return 3;
    }
    serialctl::TerminalDecoder transition;
    if (!transition.Decode({0xe4}, CP_UTF8).empty() || transition.Decode({'A'}, 936) != L"\xfffd"
                                                                                        L"A")
        return 4;
#endif
    std::cout << "Terminal decoder split UTF-8/GBK/OEM tests passed\n";
}
