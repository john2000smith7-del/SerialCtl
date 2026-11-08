#include "TerminalDecoder.h"
#include "TerminalModel.h"
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

    // Issue #59: ZMODEM rz hex frame normalization
    {
        // 1. Full ZMODEM rz handshake frame
        const std::vector<std::uint8_t> zmodemFrame = {
            '*', '*', 0x18, 'B', '0', '1', '0', '0', '0', '0', '0', '0', '2', '3', 'b', 'e', '5', '0', '\r', 0x8a, 0x11
        };
        serialctl::TerminalDecoder zdec;
        auto decoded = zdec.Decode(zmodemFrame, CP_UTF8);
        if (decoded.find(L"\xfffd") != std::wstring::npos) {
            std::cerr << "ZMODEM frame produced unexpected U+FFFD\n";
            return 5;
        }
        if (decoded.find(L"\r\n\x11") == std::wstring::npos) {
            std::cerr << "ZMODEM frame missing expected \\r\\n\\x11 ending\n";
            return 6;
        }

        // 2. Split frame across arbitrary packet boundaries
        for (size_t split = 1; split < zmodemFrame.size(); ++split) {
            serialctl::TerminalDecoder splitDec;
            std::vector<std::uint8_t> p1(zmodemFrame.begin(), zmodemFrame.begin() + split);
            std::vector<std::uint8_t> p2(zmodemFrame.begin() + split, zmodemFrame.end());
            auto actual = splitDec.Decode(p1, CP_UTF8);
            actual += splitDec.Decode(p2, CP_UTF8);
            if (actual.find(L"\xfffd") != std::wstring::npos || actual.find(L"\r\n") == std::wstring::npos) {
                std::cerr << "ZMODEM split at " << split << " failed\n";
                return 7;
            }
        }

        // 3. Negative guard: isolated 0x8A in non-ZMODEM context must retain U+FFFD
        serialctl::TerminalDecoder guardDec;
        std::vector<std::uint8_t> plainText = {'h', 'e', 'l', 'l', 'o', 0x8a, 'w', 'o', 'r', 'l', 'd'};
        auto plainDecoded = guardDec.Decode(plainText, CP_UTF8);
        if (plainDecoded.find(L"\xfffd") == std::wstring::npos) {
            std::cerr << "Isolated 0x8A failed to produce U+FFFD\n";
            return 8;
        }

        // 4. Negative guard: isolated \r\x8a without ZMODEM header must retain U+FFFD
        guardDec.Reset();
        std::vector<std::uint8_t> crNoise = {'t', 'e', 's', 't', '\r', 0x8a};
        auto crDecoded = guardDec.Decode(crNoise, CP_UTF8);
        if (crDecoded.find(L"\xfffd") == std::wstring::npos) {
            std::cerr << "Isolated \\r\\x8a without ZMODEM header failed to produce U+FFFD\n";
            return 9;
        }

        auto badCrc = zmodemFrame;
        badCrc[17] = '1';
        auto shortHeader = zmodemFrame;
        shortHeader.erase(shortHeader.begin() + 8, shortHeader.begin() + 18);
        auto longHeader = zmodemFrame;
        longHeader.insert(longHeader.begin() + 18, '0');
        for (const auto &invalid : {badCrc, shortHeader, longHeader}) {
            serialctl::TerminalDecoder decoder;
            std::wstring display;
            for (auto byte : invalid) display += decoder.Decode({byte}, CP_UTF8);
            if (display.find(L'\xfffd') == std::wstring::npos) {
                std::cerr << "Malformed ZHEX swallowed diagnostic byte\n";
                return 10;
            }
        }
    }

    const std::wstring reboot = L"\x009dnoise\r\nINIT: 正在重启\r\n"
                                 L"stopping\tssh\r[OK] stopping ssh\b!\r\n"
                                 L"\x1b[32mRebooting...\x1b[0m\r\n";
    const auto encodedReboot = serialctl::WideToMultiByte(reboot, CP_UTF8);
    std::vector<std::uint8_t> rebootBytes(encodedReboot.begin(), encodedReboot.end());
    rebootBytes.push_back(0xff);
    const auto prompt = serialctl::WideToMultiByte(L"root@petalinux:~#\r\n", CP_UTF8);
    rebootBytes.insert(rebootBytes.end(), prompt.begin(), prompt.end());
    for (const bool timestamps : {false, true}) {
        const std::wstring timestamp = timestamps ? L"12:34:56.789" : L"";
        serialctl::TerminalDecoder wholeDecoder;
        serialctl::TerminalModel whole;
        const auto expectedLog = whole.Feed(wholeDecoder.Decode(rebootBytes, CP_UTF8), timestamp).logText;
        for (size_t split = 0; split <= rebootBytes.size(); ++split) {
            serialctl::TerminalDecoder decoder;
            serialctl::TerminalModel model;
            const auto first = model.Feed(decoder.Decode({rebootBytes.begin(), rebootBytes.begin() + split}, CP_UTF8), timestamp);
            const auto second = model.Feed(decoder.Decode({rebootBytes.begin() + split, rebootBytes.end()}, CP_UTF8), timestamp);
            const auto screen = model.PlainText(timestamps);
            if (screen != whole.PlainText(timestamps) || first.logText + second.logText != expectedLog ||
                screen.find(L"INIT: 正在重启") == std::wstring::npos ||
                screen.find(L"\xfffdroot@petalinux:~#") == std::wstring::npos ||
                expectedLog.find(L"Rebooting...") == std::wstring::npos ||
                (expectedLog.find(L"12:34:56.789") != std::wstring::npos) != timestamps) {
                std::cerr << "Controlled reboot replay differs at byte boundary " << split << '\n';
                return 11;
            }
        }
    }
#endif
    std::cout << "Terminal decoder split UTF-8/GBK/OEM & ZMODEM tests passed\n";
}
