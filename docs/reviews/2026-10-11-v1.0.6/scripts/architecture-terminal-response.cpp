#include "TerminalModel.h"
#include <iomanip>
#include <iostream>
#include <string>

// Production model execution, not a Win32 window or a physical UART test.
static std::string hex(const std::string& bytes) {
    const char* alphabet = "0123456789abcdef";
    std::string out;
    for (unsigned char b : bytes) { out += alphabet[b >> 4]; out += alphabet[b & 15]; }
    return out;
}
int main() {
    serialctl::TerminalModel owner(80, 24), viewer(100, 30);
    const std::wstring query = L"\x1b[5n\x1b[6n\x1b[c\x1b[18t";
    const auto local = owner.Feed(query, L"").response;
    const auto remote = viewer.Feed(query, L"").response;
    std::cout << "owner_response_hex=" << hex(local) << '\n';
    std::cout << "remote_response_hex=" << hex(remote) << '\n';
    std::cout << "two_models_total_reply_bytes=" << local.size() + remote.size() << '\n';
    serialctl::TerminalModel replayViewer(100, 30);
    const auto replay = replayViewer.Feed(query, L"").response;
    std::cout << "historical_output_replay_response_hex=" << hex(replay) << '\n';
    if (local.empty() || remote.empty() || replay != remote || local == remote) return 1;
    std::cout << "PASS: production models both reply, different grids report different sizes, replay replies again.\n";
    std::cout << "LIMIT: no MainWindow, network, driver, or real device executed by this harness.\n";
}
