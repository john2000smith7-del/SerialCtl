#include "TcpConnection.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ws2tcpip.h>

namespace {

using serialctl::Bytes;

bool Contains(const Bytes& data, const Bytes& expected) {
    return std::search(data.begin(), data.end(), expected.begin(), expected.end()) != data.end();
}

bool ContainsText(const Bytes& data, const char* text) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(text);
    return Contains(data, Bytes(first, first + strlen(text)));
}

} // namespace

int main() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::cerr << "listener socket failed\n";
        WSACleanup();
        return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        std::cerr << "listener setup failed\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }
    int addressLength = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength);
    const std::uint16_t port = ntohs(address.sin_port);

    Bytes received;
    std::atomic<bool> serverReady{false};
    std::thread server([&] {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        serverReady = true;
        const Bytes greeting = {
            255, 253, 24,       // DO TERMINAL-TYPE
            255, 253, 31,       // DO NAWS
            255, 251, 1,        // WILL ECHO
            255, 251, 3,        // WILL SUPPRESS-GO-AHEAD
            255, 250, 24, 1, 255, 240, // TERMINAL-TYPE SEND
            'o', 'k'};
        send(client, reinterpret_cast<const char*>(greeting.data()),
            static_cast<int>(greeting.size()), 0);
        DWORD timeout = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::uint8_t buffer[512]{};
        for (;;) {
            const int count = recv(client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
            if (count <= 0) break;
            received.insert(received.end(), buffer, buffer + count);
            const Bytes resized = {255, 250, 31, 0, 132, 0, 43, 255, 240};
            if (Contains(received, resized)) break;
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
    });

    HANDLE payloadReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Bytes payload;
    std::mutex payloadMutex;
    serialctl::TcpConnection connection(L"127.0.0.1", port, true);
    std::wstring error;
    const bool started = connection.Start(
        [&](const Bytes& data) {
            std::lock_guard<std::mutex> lock(payloadMutex);
            payload.insert(payload.end(), data.begin(), data.end());
            SetEvent(payloadReady);
        },
        [](const std::wstring&, bool) {}, error);
    bool passed = started;
    if (!started) {
        std::wcerr << L"connection start failed: " << error << L'\n';
    } else {
        passed = WaitForSingleObject(payloadReady, 3000) == WAIT_OBJECT_0;
        connection.ResizeTerminal(132, 43);
    }
    if (server.joinable()) server.join();
    connection.Stop();
    CloseHandle(payloadReady);
    closesocket(listener);
    WSACleanup();

    const Bytes willTerminalType = {255, 251, 24};
    const Bytes willNaws = {255, 251, 31};
    const Bytes doEcho = {255, 253, 1};
    const Bytes doSuppressGoAhead = {255, 253, 3};
    const Bytes initialSize = {255, 250, 31, 0, 80, 0, 24, 255, 240};
    const Bytes resized = {255, 250, 31, 0, 132, 0, 43, 255, 240};
    passed = passed && Contains(received, willTerminalType) && Contains(received, willNaws) &&
        Contains(received, doEcho) && Contains(received, doSuppressGoAhead) &&
        Contains(received, initialSize) && Contains(received, resized) &&
        ContainsText(received, "xterm-256color");
    {
        std::lock_guard<std::mutex> lock(payloadMutex);
        passed = passed && payload == Bytes({'o', 'k'});
    }
    if (!passed) {
        std::cerr << "Telnet terminal negotiation test failed\n";
        return 1;
    }
    std::cout << "Telnet terminal negotiation test passed\n";
    return 0;
}
