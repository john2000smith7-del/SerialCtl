#include "GatewayFixture.h"
#include "SharedSerialConnection.h"
using namespace test;
int main() {
    Wsa wsa;
    try {
        Fixture f;
        std::vector<std::wstring> names, descriptions;
        std::wstring error;
        Expect(SharedSerialConnection::Discover(L"127.0.0.1", static_cast<std::uint16_t>(f.server.Port()), names, error,
                                                &descriptions) &&
                   names.size() == 2,
               "desktop discovery uses gateway");
        f.server.Publish("session-3", Bytes{0, 255, 1}); // output available even before subscriber handshake
        SharedSerialConnection c(L"127.0.0.1", static_cast<std::uint16_t>(f.server.Port()), L"COM3");
        std::mutex lock;
        Bytes received;
        Expect(c.Start(
                   [&](const Bytes &b) {
                       std::lock_guard<std::mutex> l(lock);
                       received.insert(received.end(), b.begin(), b.end());
                   },
                   [](const auto &, bool) {}, error),
               "desktop subscribe");
        Wait([&] {
            std::lock_guard<std::mutex> l(lock);
            return received.size() >= 3;
        });
        {
            std::lock_guard<std::mutex> l(lock);
            Expect(received == Bytes({0, 255, 1}), "first bytes retained across handshake");
        }
        Bytes all;
        for (unsigned i = 0; i < 256; ++i)
            all.push_back(static_cast<std::uint8_t>(i));
        Expect(c.Send(all, error), "desktop sends every binary byte");
        Wait([&] {
            std::lock_guard<std::mutex> l(lock);
            return received.size() == 259;
        });
        {
            std::lock_guard<std::mutex> l(lock);
            Expect(Bytes(received.begin() + 3, received.end()) == all, "desktop output exact 0..255");
        }
        c.Stop();
        for (int i = 0; i < 1100; ++i)
            f.server.Publish("session-3", Bytes{9});
        std::atomic_bool gapReported{false};
        received.clear();
        Expect(c.Start(
                   [&](const Bytes &b) {
                       std::lock_guard<std::mutex> l(lock);
                       received.insert(received.end(), b.begin(), b.end());
                   },
                   [&](const std::wstring &reason, bool) {
                       if (reason.find(L"HISTORY_GAP") != std::wstring::npos)
                           gapReported = true;
                   },
                   error),
               "new desktop selection works after expired history");
        Expect(gapReported, "history gap must be explicitly reported before live selection");
        Expect(c.Send(all, error), "live input after explicit history gap");
        Wait([&] {
            std::lock_guard<std::mutex> l(lock);
            return received.size() == all.size();
        });
        {
            std::lock_guard<std::mutex> l(lock);
            Expect(received == all, "history gap recovery preserves new live bytes");
        }
        c.Stop();
        Expect(!c.IsConnected(), "cancel joins reader");
        Expect(c.Start({}, {}, error), "desktop reconnect");
        c.Stop();
        std::cout << "Native remote client discovery, first byte, binary and reconnect passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
