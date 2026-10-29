#include "GatewayFixture.h"
#include "NetworkConnect.h"
#include "SharedSerialConnection.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

namespace {
using namespace serialctl;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void Require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
std::wstring Env(const wchar_t *name) {
    std::vector<wchar_t> value(32768);
    const DWORD n = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    return n && n < value.size() ? std::wstring(value.data(), n) : std::wstring();
}
void RecordResolverStart() {
    const auto name = Env(L"SERIALCTL_TEST_RESOLVER_COUNTER");
    Handle mapping{OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str())};
    if (!mapping.value) return;
    auto count = static_cast<volatile LONG *>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG)));
    if (count) { InterlockedIncrement(count); UnmapViewOfFile(const_cast<LONG *>(count)); }
}
SOCKET Listener(int family, std::uint16_t &port) {
    SOCKET socketHandle = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) return INVALID_SOCKET;
    if (family == AF_INET) {
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socketHandle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
            closesocket(socketHandle); return INVALID_SOCKET;
        }
        int length = sizeof(address);
        getsockname(socketHandle, reinterpret_cast<sockaddr *>(&address), &length); port = ntohs(address.sin_port);
    } else {
        sockaddr_in6 address{}; address.sin6_family = AF_INET6;
        InetPtonW(AF_INET6, L"::1", &address.sin6_addr);
        if (bind(socketHandle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
            closesocket(socketHandle); return INVALID_SOCKET;
        }
        int length = sizeof(address);
        getsockname(socketHandle, reinterpret_cast<sockaddr *>(&address), &length); port = ntohs(address.sin6_port);
    }
    if (listen(socketHandle, 2) == SOCKET_ERROR) { closesocket(socketHandle); return INVALID_SOCKET; }
    return socketHandle;
}
void TestFallback(int targetFamily) {
    std::uint16_t port = 0;
    SOCKET listener = Listener(targetFamily, port);
    Require(listener != INVALID_SOCKET, "Loopback listener unavailable");
    TcpAddresses candidates;
    auto add = [&](int family) {
        TcpAddress candidate{};
        if (family == AF_INET) {
            auto *address = reinterpret_cast<sockaddr_in *>(&candidate.address);
            address->sin_family = AF_INET; address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            candidate.length = sizeof(*address);
        } else {
            auto *address = reinterpret_cast<sockaddr_in6 *>(&candidate.address);
            address->sin6_family = AF_INET6; InetPtonW(AF_INET6, L"::1", &address->sin6_addr);
            candidate.length = sizeof(*address);
        }
        candidates.push_back(candidate);
    };
    add(targetFamily == AF_INET ? AF_INET6 : AF_INET);
    add(targetFamily);
    std::wstring error;
    const SOCKET connected = ConnectTcpSocket(candidates, port, GetTickCount64() + 1200, error);
    Require(connected != INVALID_SOCKET, "Failed to fall back to next address");
    sockaddr_storage peer{}; int length = sizeof(peer);
    Require(getpeername(connected, reinterpret_cast<sockaddr *>(&peer), &length) == 0 && peer.ss_family == targetFamily,
            "Connection did not fall back to expected address family");
    closesocket(connected); closesocket(listener);
}
ULONGLONG TestGatewayBudget() {
    std::uint16_t port = 0;
    SOCKET listener = Listener(AF_INET, port);
    Require(listener != INVALID_SOCKET, "Create gateway deadline listener");
    Handle reached{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Require(reached.value && release.value, "Create gateway deadline gates");
    std::exception_ptr serverError;
    std::thread server([&] {
        SOCKET accepted = accept(listener, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            serverError = std::make_exception_ptr(std::runtime_error("Gateway fixture accept failed"));
            return;
        }
        try {
            ws::SocketOptions(accepted, 1000);
            Bytes tail;
            const auto request = ws::ReadHeader(accepted, tail, 1000);
            const auto response = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n") +
                "Connection: Upgrade\r\nSec-WebSocket-Protocol: serialctl.v1\r\nSec-WebSocket-Accept: " +
                ws::AcceptKey(request.fields.at("sec-websocket-key")) + "\r\n\r\n";
            if (!ws::SendAll(accepted, response.data(), response.size()))
                throw std::runtime_error("Gateway fixture handshake send failed");
            ws::Stream stream(accepted, false, std::move(tail));
            const auto hello = nlohmann::json{{"type", "hello"}, {"version", 1}, {"instance", "deadline-test"}}.dump();
            if (!stream.Send(1, Bytes(hello.begin(), hello.end())))
                throw std::runtime_error("Gateway fixture hello send failed");
            stream.Receive();
            SetEvent(reached.value);
            WaitForSingleObject(release.value, INFINITE);
        } catch (...) { serverError = std::current_exception(); }
        closesocket(accepted);
    });
    struct JoinServer {
        SOCKET listener; HANDLE release; std::thread &server;
        ~JoinServer() { SetEvent(release); closesocket(listener); if (server.joinable()) server.join(); }
    } join{listener, release.value, server};
    const auto start = GetTickCount64();
    bool failed = false;
    try {
        GatewayClient client;
        client.ConnectUntil(L"127.0.0.1", port, nullptr, start + 450);
        client.Request("resources");
    } catch (const std::exception &) { failed = true; }
    const auto elapsed = GetTickCount64() - start;
    Require(WaitForSingleObject(reached.value, 0) == WAIT_OBJECT_0,
            "Gateway deadline fixture did not receive the resource request");
    Require(failed && elapsed >= 350 && elapsed < 1000,
            "TCP, handshake, hello and request exceeded their common deadline");
    SetEvent(release.value);
    server.join();
    if (serverError) std::rethrow_exception(serverError);
    return elapsed;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    using namespace serialctl;
    if (argc == 3 && std::wstring(argv[1]) == NetworkResolverArgument) {
        RecordResolverStart();
        if (std::wstring(argv[2]) == L"serialctl-blackhole.test") {
            const auto name = Env(L"SERIALCTL_TEST_RESOLVER_EVENT");
            Handle ready{OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str())};
            if (ready.value) SetEvent(ready.value);
            Handle forever{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            WaitForSingleObject(forever.value, INFINITE);
            return 1;
        }
        return RunNetworkResolver(argv[2]);
    }
    try {
        test::Wsa sockets;
        const auto suffix = std::to_wstring(GetCurrentProcessId());
        const auto counterName = L"Local\\SerialCtlResolverCounter-" + suffix;
        const auto eventName = L"Local\\SerialCtlResolverReady-" + suffix;
        Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), counterName.c_str())};
        Handle ready{CreateEventW(nullptr, TRUE, FALSE, eventName.c_str())};
        Require(mapping.value && ready.value, "Create resolver test synchronization");
        auto counter = static_cast<volatile LONG *>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG)));
        Require(counter != nullptr, "Map resolver counter");
        struct Unmap { volatile LONG *view; ~Unmap() { UnmapViewOfFile(const_cast<LONG *>(view)); } } unmap{counter};
        SetEnvironmentVariableW(L"SERIALCTL_TEST_RESOLVER_COUNTER", counterName.c_str());
        SetEnvironmentVariableW(L"SERIALCTL_TEST_RESOLVER_EVENT", eventName.c_str());
        TcpAddresses addresses; std::wstring error;
        *counter = 0;
        Require(ResolveTcpAddresses(L"127.0.0.1", GetTickCount64() + 1000, addresses, error) && *counter == 0,
                "Numeric address started a resolver child");
        Require(ResolveTcpAddresses(L"::1", GetTickCount64() + 1000, addresses, error) && *counter == 0,
                "IPv6 numeric address started a resolver child");
        TestFallback(AF_INET);
        TestFallback(AF_INET6);
        const auto gatewayMs = TestGatewayBudget();
        {
            test::Fixture fixture(7000, 7015);
            *counter = 0;
            std::uint16_t port = 0; std::vector<std::wstring> names;
            Require(SharedSerialConnection::DiscoverAuto(L"localhost", port, names, error) &&
                    port == fixture.server.Port() && names.size() == 2,
                    "Automatic discovery failed with resolved localhost");
            Require(*counter == 1, "Automatic discovery performed repeated DNS lookups");
        }
        auto blackhole = [&](bool cancelRequest) {
            ResetEvent(ready.value);
            std::atomic_bool cancel{false};
            const auto start = GetTickCount64();
            auto job = std::async(std::launch::async, [&] {
                TcpAddresses result; std::wstring reason;
                const bool ok = ResolveTcpAddresses(L"serialctl-blackhole.test", start + 400, result, reason, &cancel);
                return std::make_pair(ok, reason);
            });
            Require(WaitForSingleObject(ready.value, 3000) == WAIT_OBJECT_0, "Blackhole resolver did not start");
            const auto signaled = GetTickCount64();
            if (cancelRequest) cancel = true;
            Require(job.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Blackhole resolver did not exit");
            const auto result = job.get();
            const auto duration = GetTickCount64() - (cancelRequest ? signaled : start);
            Require(!result.first && duration < (cancelRequest ? 500 : 1000), "Resolver cancel or deadline was not bounded");
            return duration;
        };
        const auto cancelMs = blackhole(true);
        const auto timeoutMs = blackhole(false);
        DWORD before = 0, after = 0;
        GetProcessHandleCount(GetCurrentProcess(), &before);
        for (int i = 0; i < 6; ++i) blackhole(true);
        GetProcessHandleCount(GetCurrentProcess(), &after);
        Require(after <= before + 4, "Resolver process or pipe handles grew across cancellation");
        const nlohmann::json evidence{{"dnsLookupsFor16Ports", 1}, {"cancelMs", cancelMs}, {"timeoutMs", timeoutMs},
            {"gatewayTotalDeadlineMs", gatewayMs},
            {"handlesBefore", before}, {"handlesAfter", after}, {"ipv4AndIpv6Fallback", true},
            {"scope", "real helper processes and Windows loopback sockets; DNS blackhole is a controlled child"}};
        std::cout << evidence.dump() << '\n';
        wchar_t directory[32768]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", directory, 32768))
            std::ofstream(std::filesystem::path(directory) / L"network-connect.json") << evidence.dump(2);
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
