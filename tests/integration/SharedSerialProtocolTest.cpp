#include "SharedSerialConnection.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include <winsock2.h>

namespace {

std::string ReceiveLine(SOCKET socket) {
    std::string result;
    char ch = 0;
    while (recv(socket, &ch, 1, 0) == 1) {
        result.push_back(ch);
        if (ch == '\n') break;
    }
    return result;
}

bool ReceiveExact(SOCKET socket, char* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const int received = recv(socket, data + offset, static_cast<int>(size - offset), 0);
        if (received <= 0) return false;
        offset += static_cast<size_t>(received);
    }
    return true;
}

bool ReceiveFrame(SOCKET socket, char& type, std::string& payload) {
    std::array<unsigned char, 5> header{};
    if (!ReceiveExact(socket, reinterpret_cast<char*>(header.data()), header.size())) return false;
    type = static_cast<char>(header[0]);
    const std::uint32_t length = (static_cast<std::uint32_t>(header[1]) << 24) |
        (static_cast<std::uint32_t>(header[2]) << 16) |
        (static_cast<std::uint32_t>(header[3]) << 8) | static_cast<std::uint32_t>(header[4]);
    payload.assign(length, '\0');
    return length == 0 || ReceiveExact(socket, payload.data(), payload.size());
}

bool SendFrame(SOCKET socket, char type, const std::string& payload) {
    const std::array<unsigned char, 5> header{
        static_cast<unsigned char>(type),
        static_cast<unsigned char>((payload.size() >> 24) & 0xFF),
        static_cast<unsigned char>((payload.size() >> 16) & 0xFF),
        static_cast<unsigned char>((payload.size() >> 8) & 0xFF),
        static_cast<unsigned char>(payload.size() & 0xFF)};
    if (send(socket, reinterpret_cast<const char*>(header.data()), static_cast<int>(header.size()), 0) !=
        static_cast<int>(header.size()))
        return false;
    return payload.empty() ||
           send(socket, payload.data(), static_cast<int>(payload.size()), 0) == static_cast<int>(payload.size());
}

void MockServer(std::promise<std::uint16_t> portPromise) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    listen(listener, 2);
    int length = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    portPromise.set_value(ntohs(address.sin_port));

    SOCKET discovery = accept(listener, nullptr, nullptr);
    if (ReceiveLine(discovery) == "SERIALCTL/1 LIST\n") {
        const std::string ports = "SERIALCTL/1 PORTS\nCOM7\nCOM9\n.\n";
        send(discovery, ports.data(), static_cast<int>(ports.size()), 0);
    }
    closesocket(discovery);

    SOCKET session = accept(listener, nullptr, nullptr);
    if (ReceiveLine(session) == "SERIALCTL/2 OPEN COM9\n") {
        const std::string ok = "SERIALCTL/2 OK WRITE\n";
        // The reply and first frame share one write; the next frame is split.
        const std::string greeting = ok + std::string("D\0\0\0\5hello", 10) + "D";
        send(session, greeting.data(), static_cast<int>(greeting.size()), 0);
        const std::string remaining("\0\0\0\1!", 5);
        send(session, remaining.data(), static_cast<int>(remaining.size()), 0);
        char type = 0;
        std::string payload;
        if (ReceiveFrame(session, type, payload) && type == 'D' && payload == "ping") {
            SendFrame(session, 'D', "pong");
        }
    }
    shutdown(session, SD_BOTH);
    closesocket(session);
    closesocket(listener);
}

} // namespace

int main() {
    WSADATA socketData{};
    if (WSAStartup(MAKEWORD(2, 2), &socketData) != 0) return 1;

    std::promise<std::uint16_t> portPromise;
    auto portFuture = portPromise.get_future();
    std::thread server(MockServer, std::move(portPromise));
    const std::uint16_t port = portFuture.get();

    std::vector<std::wstring> ports;
    std::wstring error;
    if (!serialctl::SharedSerialConnection::Discover(L"127.0.0.1", port, ports, error) ||
        ports.size() != 2 || ports[0] != L"COM7" || ports[1] != L"COM9") {
        server.join();
        WSACleanup();
        return 2;
    }

    std::mutex mutex;
    std::condition_variable received;
    std::string payload;
    serialctl::SharedSerialConnection connection(L"127.0.0.1", port, L"COM9");
    if (!connection.Start(
            [&](const serialctl::Bytes& data) {
                std::lock_guard<std::mutex> lock(mutex);
                payload.append(reinterpret_cast<const char*>(data.data()), data.size());
                received.notify_one();
            },
            [](const std::wstring&, bool) {}, error)) {
        server.join();
        WSACleanup();
        return 3;
    }
    if (!connection.Send(serialctl::Bytes{'p', 'i', 'n', 'g'}, error)) {
        connection.Stop();
        server.join();
        WSACleanup();
        return 4;
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        received.wait_for(lock, std::chrono::seconds(2), [&] { return payload == "hello!pong"; });
    }
    connection.Stop();
    server.join();
    WSACleanup();
    return payload == "hello!pong" ? 0 : 5;
}
