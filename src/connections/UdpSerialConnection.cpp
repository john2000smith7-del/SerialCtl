#include "UdpSerialConnection.h"
#include "Win32Helpers.h"
#include <array>
#include <cstring>
#include <ws2tcpip.h>

namespace serialctl {
bool UdpSerialConnection::Start(DataCallback data, StatusCallback status, std::wstring &error) {
    Stop();
    if (cancel_ || !peerPort_) {
        error = L"UDP 连接已取消或端口无效";
        return false;
    }
    peer_ = {};
    auto *v4 = reinterpret_cast<sockaddr_in *>(&peer_);
    auto *v6 = reinterpret_cast<sockaddr_in6 *>(&peer_);
    if (InetPtonW(AF_INET, host_.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(peerPort_);
        peerLength_ = sizeof(*v4);
    } else if (InetPtonW(AF_INET6, host_.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(peerPort_);
        peerLength_ = sizeof(*v6);
    } else {
        error = L"UDP 需要明确的来源 IP 地址";
        return false;
    }
    const SOCKET channel = socket(peer_.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (channel == INVALID_SOCKET) {
        error = L"无法建立 UDP 通道：" + SocketErrorMessage(WSAGetLastError());
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(channel, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char *>(&exclusive), sizeof(exclusive));
    sockaddr_storage local{};
    local.ss_family = peer_.ss_family;
    if (local.ss_family == AF_INET)
        reinterpret_cast<sockaddr_in *>(&local)->sin_port = htons(localPort_);
    else
        reinterpret_cast<sockaddr_in6 *>(&local)->sin6_port = htons(localPort_);
    if (bind(channel, reinterpret_cast<sockaddr *>(&local), peerLength_) == SOCKET_ERROR) {
        error = L"UDP 本地端口不可用：" + SocketErrorMessage(WSAGetLastError());
        closesocket(channel);
        return false;
    }
    int length = sizeof(local);
    if (getsockname(channel, reinterpret_cast<sockaddr *>(&local), &length) == SOCKET_ERROR) {
        closesocket(channel);
        error = L"无法读取 UDP 本地端口";
        return false;
    }
    boundPort_ = ntohs(local.ss_family == AF_INET ? reinterpret_cast<sockaddr_in *>(&local)->sin_port
                                                  : reinterpret_cast<sockaddr_in6 *>(&local)->sin6_port);
    if (cancel_) {
        closesocket(channel);
        error = L"UDP 连接已取消";
        return false;
    }
    data_ = std::move(data);
    status_ = std::move(status);
    socket_ = channel;
    stopping_ = false;
    try {
        reader_ = std::thread(&UdpSerialConnection::Read, this);
    } catch (...) {
        socket_ = INVALID_SOCKET;
        stopping_ = true;
        closesocket(channel);
        data_ = {};
        status_ = {};
        error = L"无法启动 UDP 接收线程";
        return false;
    }
    if (status_)
        status_(L"UDP 原始透传已监听 " + std::to_wstring(boundPort_) + L"；等待来源 " + host_ +
                    L" 的数据。无设备查询或送达保证",
                false);
    return true;
}
void UdpSerialConnection::Stop() {
    stopping_ = true;
    if (reader_.joinable())
        reader_.join();
    std::lock_guard<std::mutex> lock(sendMutex_);
    const SOCKET old = socket_.exchange(INVALID_SOCKET);
    if (old != INVALID_SOCKET)
        closesocket(old);
    data_ = {};
    status_ = {};
}
bool UdpSerialConnection::Send(const Bytes &bytes, std::wstring &error) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (!IsConnected()) {
        error = L"UDP 通道未打开";
        return false;
    }
    if (bytes.empty() || bytes.size() > 65507) {
        error = L"UDP 单条数据必须在 1–65507 字节内；未发送，不自动拆分或重发";
        return false;
    }
    const int sent = sendto(socket_, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
                            reinterpret_cast<const sockaddr *>(&peer_), peerLength_);
    if (sent != static_cast<int>(bytes.size())) {
        error = L"UDP 发送失败：" + SocketErrorMessage(WSAGetLastError());
        return false;
    }
    return true;
}
void UdpSerialConnection::Read() {
    std::array<std::uint8_t, 65536> bytes{};
    bool first = true;
    while (!stopping_) {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(socket_, &reads);
        timeval timeout{0, 100000};
        const int selected = select(0, &reads, nullptr, nullptr, &timeout);
        if (selected == 0)
            continue;
        if (selected == SOCKET_ERROR) {
            if (status_ && !stopping_)
                status_(L"UDP 接收等待失败", true);
            break;
        }
        sockaddr_storage sender{};
        int length = sizeof(sender);
        const int count = recvfrom(socket_, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
                                   reinterpret_cast<sockaddr *>(&sender), &length);
        if (count == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET)
                continue;
            if (status_ && !stopping_)
                status_(L"UDP 数据接收失败", true);
            break;
        }
        if (count <= 0 || sender.ss_family != peer_.ss_family)
            continue;
        const bool match =
            sender.ss_family == AF_INET
                ? reinterpret_cast<sockaddr_in *>(&sender)->sin_addr.s_addr ==
                      reinterpret_cast<sockaddr_in *>(&peer_)->sin_addr.s_addr
                : std::memcmp(&reinterpret_cast<sockaddr_in6 *>(&sender)->sin6_addr,
                              &reinterpret_cast<sockaddr_in6 *>(&peer_)->sin6_addr, sizeof(in6_addr)) == 0;
        if (!match)
            continue;
        if (first) {
            first = false;
            if (status_)
                status_(L"已收到指定来源的 UDP 数据；设备身份及命令执行结果仍以实际内容为准", false);
        }
        if (data_)
            data_(Bytes(bytes.begin(), bytes.begin() + count));
    }
    stopping_ = true;
}
} // namespace serialctl
