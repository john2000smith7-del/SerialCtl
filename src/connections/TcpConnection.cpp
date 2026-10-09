#include "NetworkConnect.h"
#include "TcpConnection.h"
#include "Win32Helpers.h"

#include <algorithm>
#include <array>
#include <ws2tcpip.h>

namespace serialctl {

TcpConnection::TcpConnection(std::wstring host, std::uint16_t port, bool telnet)
    : host_(std::move(host)), port_(port), telnet_(telnet) {}

TcpConnection::~TcpConnection() {
    Stop();
}

bool TcpConnection::Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) {
    if (IsConnected()) {
        error = L"网络已经连接";
        return false;
    }

    const std::wstring portText = std::to_wstring(port_);
    SOCKET connected = ConnectTcpSocket(host_, port_, error, &cancelStarting_);
    if (connected == INVALID_SOCKET) return false;

    BOOL noDelay = TRUE;
    setsockopt(connected, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    socket_ = connected;
    onData_ = std::move(onData);
    onStatus_ = std::move(onStatus);
    stopping_ = false;
    telnetState_ = 0;
    telnetWindowSizeEnabled_ = false;
    readThread_ = std::thread(&TcpConnection::ReadLoop, this);
    if (onStatus_) {
        onStatus_((telnet_ ? L"Telnet" : L"TCP") + std::wstring(L" 已连接：") + host_ + L":" + portText, false);
    }
    return true;
}

void TcpConnection::Stop() {
    stopping_ = true;
    SOCKET socket = socket_.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
    }
    if (readThread_.joinable()) {
        readThread_.join();
    }
    if (socket != INVALID_SOCKET) closesocket(socket);
    onData_ = {};
    onStatus_ = {};
}

bool TcpConnection::Send(const Bytes& data, std::wstring& error) {
    Bytes wireData;
    if (telnet_) {
        wireData.reserve(data.size());
        for (const std::uint8_t value : data) {
            wireData.push_back(value);
            if (value == 255) wireData.push_back(value);
        }
    } else {
        wireData = data;
    }
    return SendWire(wireData, error);
}

bool TcpConnection::SendWire(const Bytes& data, std::wstring& error) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (!IsConnected()) {
        error = L"网络尚未连接";
        return false;
    }
    size_t offset = 0;
    while (offset < data.size()) {
        const int sent = send(
            socket_, reinterpret_cast<const char*>(data.data() + offset),
            static_cast<int>(data.size() - offset), 0);
        if (sent == SOCKET_ERROR) {
            error = L"网络发送失败：" + SocketErrorMessage(WSAGetLastError());
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool TcpConnection::IsConnected() const {
    return socket_ != INVALID_SOCKET;
}

void TcpConnection::ResizeTerminal(int columns, int rows) {
    terminalColumns_ = std::max(1, std::min(65535, columns));
    terminalRows_ = std::max(1, std::min(65535, rows));
    if (!telnet_ || !telnetWindowSizeEnabled_ || !IsConnected()) return;
    Bytes message;
    AppendWindowSize(message);
    std::wstring ignored;
    SendWire(message, ignored);
}

void TcpConnection::AppendWindowSize(Bytes& output) const {
    constexpr std::uint8_t IAC = 255;
    constexpr std::uint8_t SB = 250;
    constexpr std::uint8_t SE = 240;
    constexpr std::uint8_t NAWS = 31;
    output.insert(output.end(), {IAC, SB, NAWS});
    const auto appendEscaped = [&](std::uint8_t value) {
        output.push_back(value);
        if (value == IAC) output.push_back(IAC);
    };
    appendEscaped(static_cast<std::uint8_t>((terminalColumns_ >> 8) & 0xff));
    appendEscaped(static_cast<std::uint8_t>(terminalColumns_ & 0xff));
    appendEscaped(static_cast<std::uint8_t>((terminalRows_ >> 8) & 0xff));
    appendEscaped(static_cast<std::uint8_t>(terminalRows_ & 0xff));
    output.insert(output.end(), {IAC, SE});
}

void TcpConnection::ReadLoop() {
    std::array<std::uint8_t, 4096> buffer{};
    while (!stopping_) {
        const int read = recv(socket_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (read <= 0) {
            if (!stopping_ && onStatus_) {
                onStatus_(read == 0 ? L"远端已关闭连接" : L"网络读取失败：" + SocketErrorMessage(WSAGetLastError()), true);
            }
            break;
        }

        Bytes payload;
        if (telnet_) {
            Bytes reply;
            payload = DecodeTelnet(buffer.data(), static_cast<size_t>(read), reply);
            if (!reply.empty()) {
                std::wstring ignored;
                SendWire(reply, ignored);
            }
        } else {
            payload.assign(buffer.begin(), buffer.begin() + read);
        }

        if (!payload.empty() && onData_) {
            onData_(payload);
        }
    }
}

Bytes TcpConnection::DecodeTelnet(const std::uint8_t* data, size_t size, Bytes& reply) {
    constexpr std::uint8_t IAC = 255;
    constexpr std::uint8_t SE = 240;
    constexpr std::uint8_t SB = 250;
    constexpr std::uint8_t DO = 253;
    constexpr std::uint8_t DONT = 254;
    constexpr std::uint8_t WILL = 251;
    constexpr std::uint8_t WONT = 252;
    constexpr std::uint8_t ECHO = 1;
    constexpr std::uint8_t SUPPRESS_GO_AHEAD = 3;
    constexpr std::uint8_t TERMINAL_TYPE = 24;
    constexpr std::uint8_t WINDOW_SIZE = 31;
    constexpr std::uint8_t TERMINAL_TYPE_IS = 0;
    constexpr std::uint8_t TERMINAL_TYPE_SEND = 1;

    Bytes output;
    output.reserve(size);
    for (size_t index = 0; index < size; ++index) {
        const std::uint8_t current = data[index];
        if (telnetState_ == 0) {
            if (current == IAC) {
                telnetState_ = 1;
            } else {
                output.push_back(current);
            }
        } else if (telnetState_ == 1) {
            if (current == IAC) {
                output.push_back(IAC);
                telnetState_ = 0;
            } else if (current == DO || current == DONT || current == WILL || current == WONT) {
                telnetCommand_ = current;
                telnetState_ = 2;
            } else if (current == SB) {
                telnetState_ = 3;
            } else {
                telnetState_ = 0;
            }
        } else if (telnetState_ == 2) {
            if (telnetCommand_ == DO) {
                if (current == TERMINAL_TYPE || current == WINDOW_SIZE || current == SUPPRESS_GO_AHEAD) {
                    reply.insert(reply.end(), {IAC, WILL, current});
                    if (current == WINDOW_SIZE) {
                        telnetWindowSizeEnabled_ = true;
                        AppendWindowSize(reply);
                    }
                } else {
                    reply.insert(reply.end(), {IAC, WONT, current});
                }
            } else if (telnetCommand_ == DONT) {
                if (current == WINDOW_SIZE) telnetWindowSizeEnabled_ = false;
            } else if (telnetCommand_ == WILL) {
                if (current == ECHO || current == SUPPRESS_GO_AHEAD)
                    reply.insert(reply.end(), {IAC, DO, current});
                else
                    reply.insert(reply.end(), {IAC, DONT, current});
            }
            telnetState_ = 0;
        } else if (telnetState_ == 3) {
            telnetSubnegotiationOption_ = current;
            telnetState_ = 4;
        } else if (telnetState_ == 4) {
            if (current == IAC) {
                telnetState_ = 5;
            } else if (telnetSubnegotiationOption_ == TERMINAL_TYPE && current == TERMINAL_TYPE_SEND) {
                static const char terminalType[] = "xterm-256color";
                reply.insert(reply.end(), {IAC, SB, TERMINAL_TYPE, TERMINAL_TYPE_IS});
                reply.insert(reply.end(), terminalType, terminalType + sizeof(terminalType) - 1);
                reply.insert(reply.end(), {IAC, SE});
            }
        } else if (telnetState_ == 5) {
            if (current == SE) telnetState_ = 0;
            else if (current != IAC) telnetState_ = 4;
        }
    }
    return output;
}

} // namespace serialctl
