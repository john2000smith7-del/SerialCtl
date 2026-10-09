#pragma once
#include "Connection.h"
#include "Win32Helpers.h"
#include <atomic>
#include <ws2tcpip.h>

namespace serialctl {
inline SOCKET ConnectTcpSocket(const std::wstring& host, std::uint16_t port,
    std::wstring& error, const std::atomic_bool* cancel = nullptr) {
    if (cancel && cancel->load()) { error = L"连接已取消。"; return INVALID_SOCKET; }
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* addresses = nullptr;
    const std::wstring portText = std::to_wstring(port);
    const int resolved = GetAddrInfoW(host.c_str(), portText.c_str(), &hints, &addresses);
    if (resolved != 0) {
        error = L"无法解析主机名，错误码：" + std::to_wstring(resolved);
        return INVALID_SOCKET;
    }
    SOCKET connected = INVALID_SOCKET;
    const DWORD started = GetTickCount();
    int lastError = WSAETIMEDOUT;
    for (ADDRINFOW* address = addresses; address; address = address->ai_next) {
        if ((cancel && cancel->load()) || GetTickCount() - started >= 5000) break;
        SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;
        u_long nonBlocking = 1;
        if (ioctlsocket(candidate, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            closesocket(candidate);
            continue;
        }
        const int result = connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
        bool ready = result == 0;
        lastError = ready ? 0 : WSAGetLastError();
        if (!ready && lastError == WSAEWOULDBLOCK) {
            while (!(cancel && cancel->load()) && GetTickCount() - started < 5000) {
                fd_set writable{}, failed{};
                FD_ZERO(&writable); FD_ZERO(&failed);
                FD_SET(candidate, &writable); FD_SET(candidate, &failed);
                timeval interval{0, 100000};
                const int selected = select(0, nullptr, &writable, &failed, &interval);
                if (selected == 0) continue;
                int size = sizeof(lastError);
                ready = selected > 0 && getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                    reinterpret_cast<char*>(&lastError), &size) == 0 && lastError == 0;
                break;
            }
        }
        nonBlocking = 0;
        ioctlsocket(candidate, FIONBIO, &nonBlocking);
        if (ready && !(cancel && cancel->load())) { connected = candidate; break; }
        closesocket(candidate);
    }
    FreeAddrInfoW(addresses);
    if (connected == INVALID_SOCKET)
        error = cancel && cancel->load() ? L"连接已取消。" :
            L"连接失败或超时：" + SocketErrorMessage(lastError == WSAEWOULDBLOCK ? WSAETIMEDOUT : lastError);
    return connected;
}
} // namespace serialctl
