#include "GatewayFixture.h"
#include <fstream>
#include <psapi.h>
using namespace test;
int main() {
    Wsa wsa;
    try {
        Fixture f(18500, 18515);
        Peer slow(f.server.Port());
        int tinyReceiveBuffer = 4096;
        Expect(setsockopt(slow.socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&tinyReceiveBuffer),
                          sizeof(tinyReceiveBuffer)) == 0,
               "controlled stalled receive window");
        Expect(slow.Request("subscribe", "session-3", {{"after", 0}})["ok"] == true, "slow subscriber");
        GatewayClient healthy;
        healthy.Connect(L"127.0.0.1", f.server.Port());
        healthy.Request("subscribe", "session-3", {{"after", 0}});
        healthy.Live();
        std::atomic<size_t> received{0};
        std::exception_ptr failure;
        std::thread reader([&] {
            try {
                for (;;) {
                    auto e = healthy.Receive();
                    if (e.value("kind", "") == "output") {
                        auto payload = Decode64(e["data"]);
                        Expect(payload.size() == 16384, "healthy packet size");
                        for (size_t i = 0; i < payload.size(); ++i)
                            Expect(payload[i] == std::uint8_t(i), "healthy byte equality");
                        received += payload.size();
                    }
                }
            } catch (...) {
                failure = std::current_exception();
            }
        });
        try {
            Bytes bytes(16384);
            for (size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::uint8_t>(i);
            for (size_t i = 1; i <= 600; ++i) {
                f.server.Publish("session-3", bytes);
                Wait([&] { return received >= i * bytes.size(); });
            }
            Expect(received == 600 * bytes.size(),
                   "healthy subscriber receives all bytes while other socket stops reading");
            try {
                Wait([&] { return f.server.Diagnostics().dump().find("SLOW_CLIENT") != std::string::npos; });
            } catch (...) {
                std::cerr << "diagnostics=" << f.server.Diagnostics().dump() << std::endl;
                throw;
            }
        } catch (...) {
            healthy.Cancel();
            reader.join();
            throw;
        }
        healthy.Cancel();
        reader.join();
        healthy.Close();
        Expect(f.com3->IsConnected(), "slow consumer cannot stop local driver");
        std::cout << "Slow-client bounded queue isolation: 9,830,400 healthy output bytes, no drops\n";
        // Measure forwarding from Publish entry to decoded event reception (loopback,
        // controlled transports, 64-byte packets, 100 samples/load, no physical UART).
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        PROCESS_MEMORY_COUNTERS memory{};
        GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
        std::cout
            << "environment=Windows runner loopback; physical_serial_rate=not_applicable; controlled_COMs=2; handles="
            << handles << " working_set=" << memory.WorkingSetSize << '\n';
        for (size_t count : {1u, 2u, 8u, 32u}) {
            Fixture load(18600, 18615);
            std::vector<std::unique_ptr<GatewayClient>> clients;
            for (size_t i = 0; i < count; ++i) {
                auto c = std::make_unique<GatewayClient>();
                c->Connect(L"127.0.0.1", load.server.Port());
                c->Request("subscribe", "session-5", {{"after", 0}});
                clients.push_back(std::move(c));
            }
            std::vector<double> delays;
            for (int sample = 0; sample < 100; ++sample) {
                LARGE_INTEGER start, end;
                QueryPerformanceCounter(&start);
                load.server.Publish("session-5", Bytes(64, static_cast<std::uint8_t>(sample)));
                for (auto &c : clients) {
                    auto e = c->Receive();
                    Expect(e.value("kind", "") == "output" &&
                               Decode64(e["data"]) == Bytes(64, static_cast<std::uint8_t>(sample)),
                           "load byte check");
                    QueryPerformanceCounter(&end);
                    delays.push_back(double(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart);
                }
            }
            std::sort(delays.begin(), delays.end());
            auto percentile = [&](double q) {
                return delays[std::min(delays.size() - 1, static_cast<size_t>(q * delays.size()))];
            };
            std::cout << "forwarding clients=" << count << " P50_ms=" << percentile(.50)
                      << " P95_ms=" << percentile(.95) << " P99_ms=" << percentile(.99) << " samples=" << delays.size()
                      << '\n';
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
