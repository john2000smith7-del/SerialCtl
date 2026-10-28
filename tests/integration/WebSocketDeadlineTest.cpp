#include "GatewayFixture.h"
#include <chrono>
#include <future>
using namespace serialctl;
using namespace std::chrono_literals;
namespace serialctl {
struct ApiServerTestAccess {
    static size_t Clients(ApiServer& server) { std::lock_guard<std::mutex> lock(server.mutex_); return server.clients_.size(); }
};
}
namespace {
struct Pair {
    SOCKET writer = INVALID_SOCKET, reader = INVALID_SOCKET;
    Pair() {
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        test::Expect(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0, "listen");
        int length = sizeof(address); getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        std::wstring error; writer = ConnectTcpSocket(L"127.0.0.1", ntohs(address.sin_port), error);
        reader = accept(listener, nullptr, nullptr); closesocket(listener);
        test::Expect(writer != INVALID_SOCKET && reader != INVALID_SOCKET, "pair");
        ws::SocketOptions(writer); ws::SocketOptions(reader);
    }
    ~Pair() { closesocket(writer); closesocket(reader); }
};
void TimeoutCase(const Bytes& partial, bool cancellable) {
    Pair pair;
    HANDLE cancel = cancellable ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    {
        ws::Stream stream(pair.reader, false, {}, cancel); stream.FrameTimeout(80);
        test::Expect(ws::SendAll(pair.writer, partial.data(), partial.size()), "send partial frame");
        const auto start = GetTickCount64();
        bool expired = false;
        try { stream.Receive(); } catch (const ws::Error& error) { expired = std::string(error.what()) == "FRAME_TIMEOUT"; }
        test::Expect(expired && GetTickCount64() - start < 1000, "partial frame must expire");
    }
    if (cancel) CloseHandle(cancel);
}
}
int main() {
    try {
        test::Wsa wsa;
        for (bool cancellable : {false, true}) {
            TimeoutCase({0x81}, cancellable);
            TimeoutCase({0x81, 0xfe, 0}, cancellable);
            TimeoutCase({0x81, 0x81, 0, 0}, cancellable);
            TimeoutCase({0x81, 0x82, 0, 0, 0, 0, 'x'}, cancellable);
            Pair pair; HANDLE cancel = cancellable ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
            {
                ws::Stream send(pair.writer, true), read(pair.reader, false, {}, cancel);
                read.FrameTimeout(80);
                test::Expect(send.Send(1, Bytes{'x'}, false) && send.Send(9, Bytes{'p'}) && send.Send(0, Bytes{'y'}), "fragment and ping");
                test::Expect(read.Receive().opcode == 9, "ping interleaves fragment");
                // The continuation is already in the prefetch buffer. It must
                // still expire against the original message deadline.
                HANDLE timer = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                WaitForSingleObject(timer, 100); CloseHandle(timer);
                bool expired = false;
                try { read.Receive(); } catch (const ws::Error& error) { expired = std::string(error.what()) == "FRAME_TIMEOUT"; }
                test::Expect(expired, "buffered continuation cannot bypass deadline");
            }
            if (cancel) CloseHandle(cancel);
        }
        {
            Pair pair; HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            {
                ws::Stream send(pair.writer, true), read(pair.reader, false, {}, cancel); read.FrameTimeout(50);
                auto idle = std::async(std::launch::async, [&] { return read.Receive(); });
                test::Expect(idle.wait_for(100ms) == std::future_status::timeout, "idle monitoring remains legal");
                send.Send(1, Bytes{'o', 'k'});
                test::Expect(idle.get().data == Bytes({'o', 'k'}), "legal message after idle");
            }
            CloseHandle(cancel);
        }
        {
            Pair pair; std::atomic_bool cancel{false};
            auto header = std::async(std::launch::async, [&] { Bytes tail; try { ws::ReadHeader(pair.reader, tail, 1500, &cancel); } catch (const ws::Error& e) { return std::string(e.what()) == "CANCELED"; } return false; });
            cancel = true;
            test::Expect(header.wait_for(200ms) == std::future_status::ready && header.get(), "discovery handshake cancellation");
        }
        {
            // Exercise the real server deadline and capacity, including cleanup
            // without a new accept to trigger reaping.
            test::Fixture fixture(18160, 18160);
            std::vector<SOCKET> clients;
            for (size_t i = 0; i < ApiServer::MaxClients; ++i) {
                std::wstring error;
                auto socket = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(fixture.server.Port()), error);
                auto tail = ws::UpgradeClient(socket, L"127.0.0.1", fixture.server.Port());
                ws::Stream hello(socket, true, std::move(tail));
                test::Expect(hello.Receive().opcode == 1, "hello before partial frame");
                const unsigned char first = 0x81;
                test::Expect(ws::SendAll(socket, &first, 1), "send single header byte");
                clients.push_back(socket);
            }
            test::Expect(ApiServerTestAccess::Clients(fixture.server) == 128, "128 slow slots occupied");
            const auto deadline = GetTickCount64() + ws::FrameTimeoutMs + 3000;
            HANDLE timer = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            while (ApiServerTestAccess::Clients(fixture.server) && GetTickCount64() < deadline) WaitForSingleObject(timer, 10);
            CloseHandle(timer);
            test::Expect(ApiServerTestAccess::Clients(fixture.server) == 0, "slow clients reaped without a new accept");
            for (auto socket : clients) closesocket(socket);
            GatewayClient healthy; healthy.Connect(L"127.0.0.1", fixture.server.Port());
            test::Expect(healthy.Request("resources").contains("resources"), "all client slots recover after disconnect");
            fixture.server.Stop();
        }
        std::cout << "sync/overlapped partial frames, buffered fragments, ping, idle, cancel and 128-client recovery passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
