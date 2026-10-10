#include "GatewayFixture.h"
#include "SharedSerialConnection.h"
using namespace test;
int main(int argc, char **) {
    Wsa wsa;
    try {
        Fixture f(argc > 1 ? 7000 : 18100, argc > 1 ? 7015 : 18115);
        if (argc > 1) {
            std::cout << f.server.Port() << std::endl;
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line == "close COM3") {
                    f.Remove3();
                    std::cout << "closed COM3" << std::endl;
                } else if (line == "quit")
                    break;
            }
            return 0;
        }
        GatewayClient a, b, other;
        for (auto c : {&a, &b, &other})
            c->Connect(L"127.0.0.1", f.server.Port());
        a.Request("subscribe", "session-3", {{"after", 0}});
        b.Request("subscribe", "session-3", {{"after", 0}});
        other.Request("subscribe", "session-5", {{"after", 0}});
        std::atomic<size_t> desktopBytes{0};
        SharedSerialConnection desktop(L"127.0.0.1", static_cast<std::uint16_t>(f.server.Port()), L"COM3");
        std::wstring error;
        Expect(
            desktop.Start([&](const Bytes &bytes) { desktopBytes += bytes.size(); }, [](const auto &, bool) {}, error),
            "desktop alongside two AI clients");
        Bytes data{0, 255, 13, 10, 42};
        a.SendRequest("input", "session-3", {{"data", Encode64(data)}});
        b.SendRequest("input", "session-3", {{"data", Encode64(data)}});
        desktop.Send(data, error);
        Wait([&] { return f.rx3->received == 15 && desktopBytes == 15; });
        auto drain = [&](GatewayClient &c, size_t bytes) {
            Bytes output;
            while (output.size() < bytes) {
                auto event = c.Receive();
                if (event.value("kind", "") == "output") {
                    auto chunk = Decode64(event["data"]);
                    output.insert(output.end(), chunk.begin(), chunk.end());
                }
            }
            return output;
        };
        Bytes triple = data;
        triple.insert(triple.end(), data.begin(), data.end());
        triple.insert(triple.end(), data.begin(), data.end());
        Expect(drain(a, 15) == triple && drain(b, 15) == triple, "two AI clients same-COM receive and write");
        std::thread writerA([&] {
            for (int i = 0; i < 100; ++i)
                a.SendRequest("input", "session-3", {{"data", Encode64(Bytes(64, 1))}});
        });
        std::thread writerB([&] {
            for (int i = 0; i < 100; ++i)
                b.SendRequest("input", "session-3", {{"data", Encode64(Bytes(64, 2))}});
        });
        writerA.join();
        writerB.join();
        auto concurrent = drain(a, 12800);
        Expect(drain(b, 12800) == concurrent, "concurrent writers share identical ordered output");
        for (size_t offset = 0; offset < concurrent.size(); offset += 64) {
            Expect(concurrent[offset] == 1 || concurrent[offset] == 2, "writer identity");
            for (size_t i = 0; i < 64; ++i)
                Expect(concurrent[offset + i] == concurrent[offset], "complete batches never byte-interleave");
        }
        Wait([&] { return f.rx3->received == 12815 && desktopBytes == 12815; });
        Expect(f.rx5->received == 0, "COM3 ingress isolated from COM5");
        other.SendRequest("input", "session-5", {{"data", Encode64(data)}});
        Expect(drain(other, 5) == data, "COM5 separate stream");
        f.Remove3();
        other.SendRequest("input", "session-5", {{"data", Encode64(data)}});
        Expect(drain(other, 5) == data, "closing COM3 does not affect COM5");
        desktop.Stop();
        auto occupied = Reserve(18200);
        Expect(occupied != INVALID_SOCKET, "reserve default test port");
        {
            Fixture fallback(18200, 18215);
            Expect(fallback.server.Port() > 18200, "occupied default falls back within range");
        }
        closesocket(occupied);
        std::vector<SOCKET> held;
        for (unsigned p = 18300; p <= 18315; ++p) {
            auto s = Reserve(p);
            Expect(s != INVALID_SOCKET, "reserve whole range");
            held.push_back(s);
        }
        ApiServer unavailable;
        Expect(!unavailable.Start({}, error, 18300, 18315), "whole range unavailable");
        Expect(f.com5->IsConnected(), "local transport remains open without another server");
        for (auto s : held)
            closesocket(s);
        {
            Fixture firstInstance(7000, 7015), secondInstance(7000, 7015);
            std::uint16_t port = 0;
            std::vector<std::wstring> names;
            Expect(!SharedSerialConnection::DiscoverAuto(L"127.0.0.1", port, names, error, nullptr, 0),
                   "multiple instances must report ambiguity");
            Expect(error.find(L"多个") != std::wstring::npos, "ambiguity is distinguishable from no opened COM");
            Expect(SharedSerialConnection::DiscoverAuto(L"127.0.0.1", port, names, error, nullptr,
                                                        static_cast<std::uint16_t>(secondInstance.server.Port())),
                   "explicit port selects instance");
            SharedSerialConnection wrongInstance(L"127.0.0.1", static_cast<std::uint16_t>(secondInstance.server.Port()),
                                                 L"COM3", firstInstance.server.Instance());
            Expect(!wrongInstance.Start([](const Bytes &) {}, [](const auto &, bool) {}, error) &&
                       error.find(L"INSTANCE_MISMATCH") != std::wstring::npos,
                   "discovered instance pin prevents port reuse mismatch");
            std::atomic_bool cancel{true};
            Expect(!SharedSerialConnection::DiscoverAuto(L"127.0.0.1", port, names, error, &cancel),
                   "discovery cancel");
        }
        std::cout << "Two AI plus desktop, multi-COM isolation, target removal, fallback and local survival passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
