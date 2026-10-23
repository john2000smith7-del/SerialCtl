#include "UdpSerialConnection.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <ws2tcpip.h>
using namespace serialctl;
static void Require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        return 1;
    SOCKET peer = INVALID_SOCKET;
    try {
        peer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Require(peer != INVALID_SOCKET, "peer socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(bind(peer, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "peer bind");
        int length = sizeof(address);
        Require(getsockname(peer, reinterpret_cast<sockaddr *>(&address), &length) == 0, "peer address");
        const auto port = ntohs(address.sin_port);
        UdpSerialConnection client(L"127.0.0.1", port, 0);
        std::mutex mutex;
        std::condition_variable wake;
        Bytes received;
        std::atomic_uint statuses{0};
        std::wstring error;
        Require(client.Start(
                    [&](const Bytes &bytes) {
                        std::lock_guard<std::mutex> lock(mutex);
                        received = bytes;
                        wake.notify_all();
                    },
                    [&](const std::wstring &, bool) { ++statuses; }, error),
                "raw UDP open");
        Require(client.LocalPort() != 0 && client.IsConnected(), "listener state");
        Require(statuses >= 1, "waiting status missing");
        sockaddr_in destination = address;
        destination.sin_port = htons(client.LocalPort());
        const Bytes binary = {0, 13, 10, 255, 0x41, 0xe4, 0xb8, 0xad};
        Require(sendto(peer, reinterpret_cast<const char *>(binary.data()), static_cast<int>(binary.size()), 0,
                       reinterpret_cast<sockaddr *>(&destination),
                       sizeof(destination)) == static_cast<int>(binary.size()),
                "incoming datagram");
        {
            std::unique_lock<std::mutex> lock(mutex);
            Require(wake.wait_for(lock, std::chrono::seconds(3), [&] { return !received.empty(); }),
                    "raw receive timeout");
            Require(received == binary, "payload was interpreted or modified");
        }
        Require(client.Send(binary, error), "raw outgoing send");
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(peer, &reads);
        timeval timeout{3, 0};
        Require(select(0, &reads, nullptr, nullptr, &timeout) == 1, "outgoing timeout");
        char buffer[64]{};
        Require(recv(peer, buffer, sizeof(buffer), 0) == static_cast<int>(binary.size()), "outgoing length");
        Require(Bytes(buffer, buffer + binary.size()) == binary, "outgoing payload");
        Require(!client.Send({}, error) && !client.Send(Bytes(65508), error), "unsafe datagram size accepted");
        UdpSerialConnection conflict(L"127.0.0.1", port, client.LocalPort());
        Require(!conflict.Start({}, {}, error), "exclusive listener conflict hidden");
        Require(client.IsConnected(), "conflicting channel affected original connection");
        client.Stop();
        Require(!client.IsConnected() && !client.Send(binary, error), "stopped channel accepted output");
        UdpSerialConnection cancelled(L"127.0.0.1", port, 0);
        cancelled.CancelStart();
        Require(!cancelled.Start({}, {}, error), "cancel ignored");
        UdpSerialConnection invalid(L"not-an-ip", port, 0);
        Require(!invalid.Start({}, {}, error), "ambiguous peer accepted");
        closesocket(peer);
        WSACleanup();
        std::cout << "Raw UDP exact bytes, waiting state, independent failures and cancellation passed\n";
        return 0;
    } catch (const std::exception &e) {
        if (peer != INVALID_SOCKET)
            closesocket(peer);
        WSACleanup();
        std::cerr << e.what() << '\n';
        return 1;
    }
}
