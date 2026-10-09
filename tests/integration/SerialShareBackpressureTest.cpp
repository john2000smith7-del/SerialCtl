#include "SerialShareConnection.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

// A deterministic in-memory serial device replaces hardware only in this test.
// The production broadcaster, sockets, queues and lifecycle are linked unchanged.
namespace serialctl {
SerialDevice::~SerialDevice() { Close(); }
bool SerialDevice::Open(const SerialSettings&, DataCallback data, StatusCallback status, std::wstring&) {
    handle_ = reinterpret_cast<HANDLE>(1);
    onData_ = std::move(data); onStatus_ = std::move(status); return true;
}
void SerialDevice::Close() { handle_ = INVALID_HANDLE_VALUE; onData_ = {}; onStatus_ = {}; }
bool SerialDevice::IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
bool SerialDevice::Write(const Bytes& data, std::wstring&) { if (onData_) onData_(data); return true; }
}

SOCKET Connect(std::uint16_t port, bool slow) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (slow) { int small = 1024; setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&small), sizeof(small)); }
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { closesocket(s); return INVALID_SOCKET; }
    send(s, "x", 1, 0); // raw protocol; no receive timeout required for handshake
    return s;
}
int main() {
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int length = sizeof(address); getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    const auto port = ntohs(address.sin_port); closesocket(probe);
    std::mutex mutex; std::condition_variable ready; int connections = 0;
    serialctl::SerialSettings settings; settings.portName = L"TEST";
    serialctl::SerialShareConnection service(settings, port);
    std::wstring error;
    if (!service.Start([](const serialctl::Bytes&) {}, [&](const std::wstring& status, bool) {
            if (status.find(L"远程客户端已连接") != std::wstring::npos) {
                std::lock_guard<std::mutex> lock(mutex); ++connections; ready.notify_all();
            }
        }, error)) return 2;
    SOCKET slow = Connect(port, true), fast = Connect(port, false);
    if (slow == INVALID_SOCKET || fast == INVALID_SOCKET) { service.Stop(); return 3; }
    std::atomic<size_t> received{0};
    std::thread reader([&] { char data[32768]; int count;
        while ((count = recv(fast, data, sizeof(data), 0)) > 0) received += static_cast<size_t>(count);
    });
    bool initialized;
    { std::unique_lock<std::mutex> lock(mutex); initialized = ready.wait_for(lock, std::chrono::seconds(3), [&] { return connections == 2; }); }
    std::cerr << "Clients initialized: " << initialized << '\n';
    serialctl::Bytes chunk(16384, 'a');
    const auto started = std::chrono::steady_clock::now();
    if (initialized) for (int i = 0; i < 512; ++i) { service.Send(chunk, error); Sleep(2); }
    const auto duration = std::chrono::steady_clock::now() - started;
    std::cerr << "Serial input completed; received " << received.load() << " bytes\n";
    for (int i = 0; i < 100 && received < chunk.size() * 512; ++i) Sleep(20);
    std::cerr << "Stopping shared service\n";
    service.Stop();
    std::cerr << "Shared service stopped\n";
    shutdown(fast, SD_BOTH); reader.join(); closesocket(fast); closesocket(slow);
    WSACleanup();
    if (!initialized || duration > std::chrono::seconds(5) || received < chunk.size() * 512) return 4;
    std::cout << "Slow-client isolation and shutdown tests passed\n";
    return 0;
}
