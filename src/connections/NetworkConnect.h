#pragma once
#include "Connection.h"
#include <atomic>
#include <ws2tcpip.h>

namespace serialctl {
struct TcpAddress {
    sockaddr_storage address{};
    int length = 0;
};
using TcpAddresses = std::vector<TcpAddress>;

// The same monotonic deadline covers name lookup and all candidate addresses.
bool ResolveTcpAddresses(const std::wstring &host, ULONGLONG deadline, TcpAddresses &addresses,
                         std::wstring &error, const std::atomic_bool *cancel = nullptr);
SOCKET ConnectTcpSocket(const TcpAddresses &addresses, std::uint16_t port, ULONGLONG deadline,
                        std::wstring &error, const std::atomic_bool *cancel = nullptr);
SOCKET ConnectTcpSocket(const std::wstring &host, std::uint16_t port, std::wstring &error,
                        const std::atomic_bool *cancel = nullptr, DWORD timeoutMs = 5000);

// Private child mode: synchronous Windows name resolution runs in a process
// the parent can cancel and reap even on Windows 7. Writes a bounded binary
// result to its inherited stdout pipe; does not start the GUI.
int RunNetworkResolver(const std::wstring &host);
inline constexpr wchar_t NetworkResolverArgument[] = L"--serialctl-network-resolver";
} // namespace serialctl
