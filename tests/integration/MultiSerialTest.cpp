#include "SerialShareConnection.h"
#include "SharedSerialConnection.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>

namespace serialctl {
SerialDevice::~SerialDevice() { Close(); }
bool SerialDevice::Open(const SerialSettings&, DataCallback data, StatusCallback status, std::wstring&) {
    handle_ = reinterpret_cast<HANDLE>(1); onData_ = std::move(data); onStatus_ = std::move(status); return true;
}
void SerialDevice::Close() { handle_ = INVALID_HANDLE_VALUE; onData_ = {}; onStatus_ = {}; }
bool SerialDevice::IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
bool SerialDevice::Write(const Bytes& bytes, std::wstring&) { if (onData_) onData_(bytes); return true; }
}
void Expect(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
SOCKET Reserve(unsigned port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); BOOL exclusive = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(sock, 8) != 0) { closesocket(sock); return INVALID_SOCKET; }
    return sock;
}
SOCKET Connect(unsigned port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(static_cast<u_short>(port));
    Expect(connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "test client connect");
    DWORD timeout = 1500; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    return sock;
}
std::string ReplyLine(SOCKET socket) {
    std::string line; char c;
    while (line.size() < 4096 && recv(socket, &c, 1, 0) == 1) { line += c; if (c == '\n') break; }
    return line;
}
int ServeFixture() {
    serialctl::SerialSettings a,b; a.portName=L"COM3"; b.portName=L"COM5";
    serialctl::SerialShareConnection first(a,0), second(b,0); std::wstring error;
    if (!first.Start({}, {}, error) || !second.Start({}, {}, error)) return 2;
    std::cout << first.SharedPort() << std::endl;
    std::string command;
    while (std::getline(std::cin,command)) {
        if (command=="close COM3") { first.Stop(); std::cout << "closed COM3" << std::endl; }
        else if (command=="quit") break;
    }
    second.Stop(); first.Stop(); return 0;
}
int main(int argc, char**) {
    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2), &wsa);
    if (argc > 1) { int result=ServeFixture(); WSACleanup(); return result; }
    int result = 0;
    try {
        unsigned base = 18000;
        SOCKET occupied = INVALID_SOCKET;
        for (; base < 20000; base += 16) { occupied = Reserve(base); if (occupied != INVALID_SOCKET) break; }
        Expect(occupied != INVALID_SOCKET, "reserve preferred port");
        serialctl::SerialSettings a, b; a.portName = L"COM3"; b.portName = L"COM5";
        serialctl::SerialShareConnection com3(a, static_cast<std::uint16_t>(base)), com5(b, static_cast<std::uint16_t>(base));
        std::atomic<size_t> received3{0}, received5{0}, remote3{0}, remote5{0}, remote5b{0}; std::wstring error;
        Expect(com3.Start([&](const serialctl::Bytes& bytes) { received3 += bytes.size(); }, {}, error), "open COM3");
        Expect(com5.Start([&](const serialctl::Bytes& bytes) { received5 += bytes.size(); }, {}, error), "open COM5");
        Expect(com3.SharedPort() > base && com3.SharedPort() < base + 16 && com3.SharedPort() == com5.SharedPort(), "fallback and one service");
        std::vector<std::wstring> names;
        Expect(serialctl::SharedSerialConnection::Discover(L"127.0.0.1", com3.SharedPort(), names, error), "discover list");
        Expect(names.size() == 2 && names[0] == L"COM3" && names[1] == L"COM5", "named ports listed");
        std::vector<std::wstring> descriptions;
        names.clear(); Expect(serialctl::SharedSerialConnection::Discover(L"127.0.0.1", com3.SharedPort(), names, error, &descriptions), "metadata query");
        Expect(descriptions.size()==2 && descriptions[1].find(L"115200") != std::wstring::npos, "metadata retains COM parameters");
        SOCKET raw = Connect(com3.SharedPort()); send(raw, "x", 1, 0);
        Expect(ReplyLine(raw) == "SERIALCTL/2 ERR SELECT_REQUIRED\n", "ambiguous raw TCP rejected"); closesocket(raw);
        Expect(received3 == 0 && received5 == 0, "ambiguous raw data never enters a COM");
        SOCKET legacy = Connect(com3.SharedPort());
        const std::string open = "SERIALCTL/1 OPEN COM3\nlegacy";
        send(legacy, open.data(), static_cast<int>(open.size()), 0);
        Expect(ReplyLine(legacy) == "SERIALCTL/1 OK\n", "V1 named OPEN remains compatible");
        char echoed[6]{}; size_t count=0;
        while (count<sizeof(echoed)) { int read=recv(legacy, echoed+count, static_cast<int>(sizeof(echoed)-count),0); Expect(read>0,"V1 binary tail received"); count += static_cast<size_t>(read); }
        Expect(std::string(echoed,6)=="legacy" && received3==6, "coalesced OPEN data retained");
        shutdown(legacy, SD_BOTH); closesocket(legacy); received3=0;
        serialctl::SharedSerialConnection first(L"127.0.0.1", com3.SharedPort(), L"COM3"), second(L"127.0.0.1", com5.SharedPort(), L"COM5"), third(L"127.0.0.1", com5.SharedPort(), L"com5");
        Expect(first.Start([&](const serialctl::Bytes& bytes) { remote3 += bytes.size(); }, {}, error), "remote COM3");
        Expect(second.Start([&](const serialctl::Bytes& bytes) { remote5 += bytes.size(); }, {}, error), "remote COM5");
        Expect(third.Start([&](const serialctl::Bytes& bytes) { remote5b += bytes.size(); }, {}, error), "second writable client");
        serialctl::Bytes data{0,1,255,10,13};
        Expect(second.Send(data, error), "write COM5");
        for (int i=0;i<100 && remote5b < data.size(); ++i) Sleep(10);
        Expect(received5 == data.size() && received3 == 0 && remote3 == 0 && remote5 == data.size() && remote5b == data.size(), "binary streams isolated and broadcast within COM5");
        Expect(third.Send(data,error), "all clients may write");
        for (int i=0;i<100 && received5 < data.size()*2; ++i) Sleep(10);
        Expect(received5 == data.size()*2, "no writer lease");
        com3.Stop(); first.Stop();
        Expect(second.Send(data,error), "COM5 remains usable after COM3 closes");
        for (int i=0;i<100 && received5 < data.size()*3; ++i) Sleep(10);
        Expect(received5 == data.size()*3, "closing another COM preserves stream");
        names.clear(); Expect(serialctl::SharedSerialConnection::Discover(L"127.0.0.1", com5.SharedPort(), names, error) && names.size()==1 && names[0]==L"COM5", "discovery updates after close");
        second.Stop(); third.Stop(); com5.Stop(); closesocket(occupied);
        // If every fallback is unavailable, serial open must still succeed locally.
        std::vector<SOCKET> held;
        for (unsigned p=base+32; p<base+48; ++p) { SOCKET socket=Reserve(p); Expect(socket != INVALID_SOCKET,"reserve full fallback range"); held.push_back(socket); }
        serialctl::SerialShareConnection local(a, static_cast<std::uint16_t>(base+32));
        Expect(local.Start({}, {}, error) && local.IsConnected() && local.SharedPort()==0, "local survives unavailable sharing ports");
        Expect(local.Send(data,error), "local write without gateway"); local.Stop();
        for (SOCKET socket:held) closesocket(socket);
        std::cout << "Multi-COM isolation, discovery, port fallback, all-writer and local-only tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    WSACleanup(); return result;
}
