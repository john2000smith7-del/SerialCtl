#include "SharedSerialConnection.h"
#include "Win32Helpers.h"

#include <array>
#include <sstream>
#include <ws2tcpip.h>

namespace serialctl {
namespace {

SOCKET ConnectSocket(const std::wstring& host, std::uint16_t port, std::wstring& error) {
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* addresses = nullptr;
    const std::wstring portText = std::to_wstring(port);
    const int resolveResult = GetAddrInfoW(host.c_str(), portText.c_str(), &hints, &addresses);
    if (resolveResult != 0) {
        error = L"无法解析来源电脑，错误码：" + std::to_wstring(resolveResult);
        return INVALID_SOCKET;
    }

    SOCKET connected = INVALID_SOCKET;
    for (ADDRINFOW* current = addresses; current; current = current->ai_next) {
        SOCKET candidate = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;
        u_long nonBlocking = 1;
        ioctlsocket(candidate, FIONBIO, &nonBlocking);
        const int connectResult = connect(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen));
        bool ready = connectResult == 0;
        if (!ready && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable{};
            FD_ZERO(&writable);
            FD_SET(candidate, &writable);
            timeval timeout{1, 500000};
            if (select(0, nullptr, &writable, nullptr, &timeout) > 0) {
                int socketError = 0;
                int errorLength = sizeof(socketError);
                ready = getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                    reinterpret_cast<char*>(&socketError), &errorLength) == 0 && socketError == 0;
            }
        }
        nonBlocking = 0;
        ioctlsocket(candidate, FIONBIO, &nonBlocking);
        if (ready) {
            connected = candidate;
            break;
        }
        closesocket(candidate);
    }
    FreeAddrInfoW(addresses);
    if (connected == INVALID_SOCKET)
        error = L"无法连接共享服务：" + SocketErrorMessage(WSAGetLastError());
    return connected;
}

bool SendAll(SOCKET socket, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const int sent = send(socket, text.data() + offset, static_cast<int>(text.size() - offset), 0);
        if (sent == SOCKET_ERROR) return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool SendBytes(SOCKET socket, const std::uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const int sent = send(socket, reinterpret_cast<const char*>(data + offset),
            static_cast<int>(size - offset), 0);
        if (sent == SOCKET_ERROR || sent == 0) return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool ReceiveExact(SOCKET socket, std::uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const int received = recv(socket, reinterpret_cast<char*>(data + offset),
            static_cast<int>(size - offset), 0);
        if (received <= 0) return false;
        offset += static_cast<size_t>(received);
    }
    return true;
}

bool ReceiveControlReply(SOCKET socket, std::string& reply, std::wstring& error) {
    DWORD timeout = 1800;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::array<char, 2048> buffer{};
    while (reply.size() < 16384) {
        const int read = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (read == 0) break;
        if (read == SOCKET_ERROR) {
            error = L"共享服务响应超时或读取失败。";
            return false;
        }
        reply.append(buffer.data(), static_cast<size_t>(read));
        const bool completeV2 = (reply.rfind("SERIALCTL/2 OK ", 0) == 0 ||
            reply.rfind("SERIALCTL/2 ERR ", 0) == 0) && reply.find('\n') != std::string::npos;
        if (reply.find("\n.\n") != std::string::npos || completeV2 ||
            reply.find("SERIALCTL/1 OK\n") == 0 || reply.find("SERIALCTL/1 ERR ") == 0) break;
    }
    return true;
}

} // namespace

SharedSerialConnection::SharedSerialConnection(
    std::wstring host, std::uint16_t port, std::wstring serialName)
    : host_(std::move(host)), port_(port), serialName_(std::move(serialName)) {}

SharedSerialConnection::~SharedSerialConnection() {
    Stop();
}

bool SharedSerialConnection::Discover(
    const std::wstring& host,
    std::uint16_t port,
    std::vector<std::wstring>& serialNames,
    std::wstring& error) {
    serialNames.clear();
    SOCKET connected = ConnectSocket(host, port, error);
    if (connected == INVALID_SOCKET) return false;
    if (!SendAll(connected, "SERIALCTL/1 LIST\n")) {
        error = L"发送串口查询请求失败。";
        closesocket(connected);
        return false;
    }
    std::string reply;
    const bool received = ReceiveControlReply(connected, reply, error);
    closesocket(connected);
    if (!received) return false;
    const std::string header = "SERIALCTL/1 PORTS\n";
    if (reply.compare(0, header.size(), header) != 0) {
        error = L"对端不是可发现串口的 SerialCtl 共享服务。";
        return false;
    }
    std::istringstream lines(reply.substr(header.size()));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == ".") break;
        if (!line.empty()) {
            const auto* data = reinterpret_cast<const std::uint8_t*>(line.data());
            serialNames.push_back(MultiByteToWide(data, line.size(), CP_UTF8));
        }
    }
    if (serialNames.empty()) {
        error = L"来源电脑当前没有已打开并共享的串口。";
        return false;
    }
    return true;
}

bool SharedSerialConnection::Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) {
    if (IsConnected()) {
        error = L"共享串口已经连接";
        return false;
    }
    SOCKET connected = ConnectSocket(host_, port_, error);
    if (connected == INVALID_SOCKET) return false;
    const std::string request = "SERIALCTL/2 OPEN " + WideToMultiByte(serialName_, CP_UTF8) + "\n";
    if (!SendAll(connected, request)) {
        error = L"发送共享串口连接请求失败。";
        closesocket(connected);
        return false;
    }
    std::string reply;
    if (!ReceiveControlReply(connected, reply, error)) {
        closesocket(connected);
        return false;
    }
    const std::string writeReply = "SERIALCTL/2 OK WRITE\n";
    const std::string readReply = "SERIALCTL/2 OK READ\n";
    const bool writable = reply.compare(0, writeReply.size(), writeReply) == 0;
    const bool readOnly = reply.compare(0, readReply.size(), readReply) == 0;
    if (!writable && !readOnly) {
        error = L"来源电脑拒绝了该串口连接。";
        closesocket(connected);
        return false;
    }
    DWORD noTimeout = 0;
    setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
    BOOL noDelay = TRUE;
    setsockopt(connected, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    socket_ = connected;
    onData_ = std::move(onData);
    onStatus_ = std::move(onStatus);
    stopping_ = false;
    readThread_ = std::thread(&SharedSerialConnection::ReadLoop, this);
    if (onStatus_)
        onStatus_(L"已连接共享串口：" + serialName_ + L" · 可直接读写", false);
    return true;
}

void SharedSerialConnection::Stop() {
    stopping_ = true;
    SOCKET current = socket_;
    socket_ = INVALID_SOCKET;
    if (current != INVALID_SOCKET) {
        shutdown(current, SD_BOTH);
        closesocket(current);
    }
    if (readThread_.joinable()) readThread_.join();
    onData_ = {};
    onStatus_ = {};
}

bool SharedSerialConnection::Send(const Bytes& data, std::wstring& error) {
    if (!IsConnected()) {
        error = L"共享串口尚未连接";
        return false;
    }
    return SendFrame('D', data, error);
}

bool SharedSerialConnection::IsConnected() const {
    return socket_ != INVALID_SOCKET;
}

void SharedSerialConnection::ReadLoop() {
    while (!stopping_) {
        std::array<std::uint8_t, 5> header{};
        if (!ReceiveExact(socket_, header.data(), header.size())) {
            if (!stopping_ && onStatus_)
                onStatus_(L"共享端已关闭连接", true);
            break;
        }
        const std::uint32_t networkLength = (static_cast<std::uint32_t>(header[1]) << 24) |
            (static_cast<std::uint32_t>(header[2]) << 16) |
            (static_cast<std::uint32_t>(header[3]) << 8) | static_cast<std::uint32_t>(header[4]);
        if (networkLength > 1024 * 1024) {
            if (onStatus_) onStatus_(L"共享服务发送了无效数据帧", true);
            break;
        }
        Bytes payload(networkLength);
        if (networkLength && !ReceiveExact(socket_, payload.data(), payload.size())) break;
        if (header[0] == 'D') {
            if (onData_) onData_(payload);
        } else if (header[0] == 'C') {
            const std::string message(payload.begin(), payload.end());
            if (message == "READ_ONLY" && onStatus_)
                onStatus_(L"远端仍在使用旧版串口互斥协议，请升级远端 SerialCtl", true);
        }
    }
}

bool SharedSerialConnection::SendFrame(char type, const Bytes& payload, std::wstring& error) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (!IsConnected()) {
        error = L"共享串口尚未连接";
        return false;
    }
    if (payload.size() > 1024 * 1024) {
        error = L"共享串口发送数据过大";
        return false;
    }
    std::array<std::uint8_t, 5> header{
        static_cast<std::uint8_t>(type),
        static_cast<std::uint8_t>((payload.size() >> 24) & 0xFF),
        static_cast<std::uint8_t>((payload.size() >> 16) & 0xFF),
        static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF),
        static_cast<std::uint8_t>(payload.size() & 0xFF)};
    if (!SendBytes(socket_, header.data(), header.size()) ||
        (!payload.empty() && !SendBytes(socket_, payload.data(), payload.size()))) {
        error = L"共享串口发送失败：" + SocketErrorMessage(WSAGetLastError());
        return false;
    }
    return true;
}

} // namespace serialctl
