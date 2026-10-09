#include "NetworkConnect.h"
#include "SharedSerialConnection.h"
#include "Win32Helpers.h"

#include <array>
#include <sstream>
#include <algorithm>
#include <future>
#include <ws2tcpip.h>

namespace serialctl {
namespace {

bool SendAll(SOCKET socket, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const int sent = send(socket, text.data() + offset, static_cast<int>(text.size() - offset), 0);
        if (sent <= 0) return false;
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

bool ReceiveExact(SOCKET socket, Bytes& buffered, std::uint8_t* data, size_t size, const std::atomic_bool& stopping) {
    const size_t bufferedCount = std::min(size, buffered.size());
    std::copy_n(buffered.begin(), bufferedCount, data);
    buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(bufferedCount));
    size_t offset = bufferedCount;
    while (offset < size) {
        const int received = recv(socket, reinterpret_cast<char*>(data + offset),
            static_cast<int>(size - offset), 0);
        if (received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT && !stopping) continue;
        if (received <= 0 || stopping) return false;
        offset += static_cast<size_t>(received);
    }
    return true;
}

bool ReceiveControlReply(SOCKET socket, std::string& reply, std::wstring& error, bool includeMetadata = false) {
    DWORD timeout = 100;
    const DWORD started = GetTickCount();
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::array<char, 2048> buffer{};
    while (reply.size() < 65536 && GetTickCount() - started < 1400) {
        const int read = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (read == 0) break;
        if (read == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAETIMEDOUT) continue;
            error = L"共享服务响应超时或读取失败。";
            return false;
        }
        reply.append(buffer.data(), static_cast<size_t>(read));
        const bool completeV2 = (reply.rfind("SERIALCTL/2 OK ", 0) == 0 ||
            reply.rfind("SERIALCTL/2 ERR ", 0) == 0) && reply.find('\n') != std::string::npos;
        if ((!includeMetadata && reply.find("\n.\n") != std::string::npos) || completeV2 ||
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
    std::wstring& error, std::vector<std::wstring>* descriptions) {
    serialNames.clear();
    if (descriptions) descriptions->clear();
    SOCKET connected = ConnectTcpSocket(host, port, error, nullptr, 500);
    if (connected == INVALID_SOCKET) return false;
    if (!SendAll(connected, "SERIALCTL/1 LIST\n")) {
        error = L"发送串口查询请求失败。";
        closesocket(connected);
        return false;
    }
    std::string reply;
    const bool received = ReceiveControlReply(connected, reply, error, descriptions != nullptr);
    closesocket(connected);
    if (!received) return false;
    const std::string header = "SERIALCTL/1 PORTS\n";
    if (reply.compare(0, header.size(), header) != 0 || reply.find("\n.\n") == std::string::npos) {
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
    if (descriptions) {
        *descriptions = serialNames;
        const auto start = reply.find("\n.\nSERIALCTL_INFO\t3\n");
        if (start != std::string::npos) {
            std::istringstream metadata(reply.substr(start + std::string("\n.\nSERIALCTL_INFO\t3\n").size()));
            while (std::getline(metadata, line)) {
                std::vector<std::string> fields; std::istringstream record(line); std::string field;
                while (std::getline(record, field, '\t')) fields.push_back(field);
                if (fields.size() != 8 || fields[7] != "open") continue;
                const auto name = MultiByteToWide(reinterpret_cast<const std::uint8_t*>(fields[0].data()), fields[0].size(), CP_UTF8);
                auto found = std::find(serialNames.begin(), serialNames.end(), name);
                if (found == serialNames.end()) continue;
                const auto numeric = [](const std::string& value) { return !value.empty() && value.size() <= 10 && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }); };
                if (!numeric(fields[1]) || !numeric(fields[6])) continue;
                (*descriptions)[static_cast<size_t>(found - serialNames.begin())] = name + L" · " +
                    MultiByteToWide(reinterpret_cast<const std::uint8_t*>(fields[1].data()), fields[1].size(), CP_UTF8) + L" · " +
                    MultiByteToWide(reinterpret_cast<const std::uint8_t*>(fields[6].data()), fields[6].size(), CP_UTF8) + L" 客户端";
            }
        }
    }
    if (serialNames.empty()) {
        error = L"来源电脑当前没有已打开并共享的串口。";
        return false;
    }
    return true;
}

bool SharedSerialConnection::DiscoverAuto(const std::wstring& host, std::uint16_t& port,
    std::vector<std::wstring>& names, std::wstring& error, const std::atomic_bool* cancel, std::uint16_t explicitPort, std::vector<std::wstring>* descriptions) {
    names.clear();
    if (descriptions) descriptions->clear();
    if (cancel && cancel->load()) { error = L"查询已取消"; return false; }
    struct Result { bool found = false; std::uint16_t port = 0; std::vector<std::wstring> names; std::vector<std::wstring> descriptions; };
    std::vector<std::future<Result>> queries;
    const unsigned first = explicitPort ? explicitPort : 7000;
    const unsigned last = explicitPort ? explicitPort : 7015;
    for (unsigned candidate = first; candidate <= last; ++candidate) {
        queries.push_back(std::async(std::launch::async, [host, candidate, cancel] {
            Result result;
            if (cancel && cancel->load()) return result;
            result.port = static_cast<std::uint16_t>(candidate);
            std::wstring ignored;
            result.found = Discover(host, result.port, result.names, ignored, &result.descriptions);
            return result;
        }));
    }
    bool found = false;
    for (auto& query : queries) {
        auto result = query.get();
        if (!found && result.found) { found = true; port = result.port; names = std::move(result.names); if (descriptions) *descriptions = std::move(result.descriptions); }
    }
    if (cancel && cancel->load()) { error = L"查询已取消"; return false; }
    if (!found) error = L"未发现共享串口，请检查来源程序、IP 和防火墙。";
    return found;
}

bool SharedSerialConnection::Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) {
    if (IsConnected()) {
        error = L"共享串口已经连接";
        return false;
    }
    SOCKET connected = ConnectTcpSocket(host_, port_, error, &cancelStarting_);
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
    DWORD sendTimeout = 1000;
    setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
    DWORD noTimeout = 200;
    setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
    BOOL noDelay = TRUE;
    setsockopt(connected, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    const size_t headerEnd = reply.find('\n') + 1;
    receivedBuffer_.assign(reply.begin() + static_cast<std::ptrdiff_t>(headerEnd), reply.end());
    socket_ = connected;
    onData_ = std::move(onData);
    onStatus_ = std::move(onStatus);
    stopping_ = false;
    readEnded_ = false;
    readThread_ = std::thread(&SharedSerialConnection::ReadLoop, this);
    if (onStatus_)
        onStatus_(L"已连接共享串口：" + serialName_ + L" · 可直接读写", false);
    return true;
}

void SharedSerialConnection::Stop() {
    stopping_ = true;
    SOCKET current = socket_.exchange(INVALID_SOCKET);
    if (current != INVALID_SOCKET) {
        shutdown(current, SD_BOTH);
    }
    if (readThread_.joinable()) readThread_.join();
    if (current != INVALID_SOCKET) closesocket(current);
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
    return socket_ != INVALID_SOCKET && !readEnded_;
}

void SharedSerialConnection::ReadLoop() {
    while (!stopping_) {
        std::array<std::uint8_t, 5> header{};
        if (!ReceiveExact(socket_, receivedBuffer_, header.data(), header.size(), stopping_)) {
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
        if (networkLength && !ReceiveExact(socket_, receivedBuffer_, payload.data(), payload.size(), stopping_)) break;
        if (header[0] == 'D') {
            if (onData_) onData_(payload);
        } else if (header[0] == 'C') {
            const std::string message(payload.begin(), payload.end());
            if (message == "READ_ONLY" && onStatus_)
                onStatus_(L"远端仍在使用旧版串口互斥协议，请升级远端 SerialCtl", true);
        }
    }
    readEnded_ = true;
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
