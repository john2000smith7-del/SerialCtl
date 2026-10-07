#include "SerialShareConnection.h"
#include "Win32Helpers.h"

#include <algorithm>
#include <array>
#include <string>

namespace serialctl {
namespace {

bool SendAll(SOCKET socket, const std::uint8_t* data, size_t size) {
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

bool SendFrame(SOCKET socket, char type, const Bytes& payload) {
    if (payload.size() > 1024 * 1024) return false;
    const std::array<std::uint8_t, 5> header{
        static_cast<std::uint8_t>(type),
        static_cast<std::uint8_t>((payload.size() >> 24) & 0xFF),
        static_cast<std::uint8_t>((payload.size() >> 16) & 0xFF),
        static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF),
        static_cast<std::uint8_t>(payload.size() & 0xFF)};
    return SendAll(socket, header.data(), header.size()) &&
        (payload.empty() || SendAll(socket, payload.data(), payload.size()));
}

} // namespace

SerialShareConnection::SerialShareConnection(SerialSettings settings, std::uint16_t listenPort)
    : settings_(std::move(settings)), listenPort_(listenPort) {}

SerialShareConnection::~SerialShareConnection() {
    Stop();
}

bool SerialShareConnection::Start(
    DataCallback onData,
    StatusCallback onStatus,
    std::wstring& error) {
    if (IsConnected()) {
        error = L"串口共享已经启动";
        return false;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = L"创建监听套接字失败：" + SocketErrorMessage(WSAGetLastError());
        return false;
    }

    BOOL reuseAddress = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(listenPort_);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        error = L"监听端口失败：" + SocketErrorMessage(WSAGetLastError());
        closesocket(listener);
        return false;
    }

    onData_ = std::move(onData);
    onStatus_ = std::move(onStatus);
    stopping_ = false;
    listener_ = listener;

    if (!device_.Open(
            settings_,
            [this](const Bytes& data) {
                if (onData_) {
                    onData_(data);
                }
                Broadcast(data);
            },
            onStatus_,
            error)) {
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
        return false;
    }

    acceptThread_ = std::thread(&SerialShareConnection::AcceptLoop, this);
    if (onStatus_) {
        onStatus_(L"正在共享 " + settings_.portName + L"，TCP 端口 " + std::to_wstring(listenPort_), false);
    }
    return true;
}

void SerialShareConnection::Stop() {
    stopping_ = true;
    SOCKET listener = listener_;
    listener_ = INVALID_SOCKET;
    if (listener != INVALID_SOCKET) {
        closesocket(listener);
    }

    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients = clients_;
    }
    for (const auto& client : clients) {
        if (client->socket != INVALID_SOCKET) {
            shutdown(client->socket, SD_BOTH);
            closesocket(client->socket);
            client->socket = INVALID_SOCKET;
        }
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    for (auto& thread : clientThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    clientThreads_.clear();
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.clear();
    }
    device_.Close();
    onData_ = {};
    onStatus_ = {};
}

bool SerialShareConnection::Send(const Bytes& data, std::wstring& error) {
    return device_.Write(data, error);
}

bool SerialShareConnection::IsConnected() const {
    return listener_ != INVALID_SOCKET && device_.IsOpen();
}

void SerialShareConnection::AcceptLoop() {
    while (!stopping_) {
        SOCKET accepted = accept(listener_, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            if (!stopping_ && onStatus_) {
                onStatus_(L"接受客户端失败：" + SocketErrorMessage(WSAGetLastError()), true);
            }
            break;
        }
        BOOL noDelay = TRUE;
        setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        auto client = std::make_shared<Client>();
        client->socket = accepted;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.push_back(client);
            clientThreads_.emplace_back(&SerialShareConnection::ClientLoop, this, client);
        }
    }
}

void SerialShareConnection::ClientLoop(const std::shared_ptr<Client>& client) {
    std::array<std::uint8_t, 4096> buffer{};
    DWORD handshakeTimeout = 400;
    setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&handshakeTimeout), sizeof(handshakeTimeout));
    std::string first;
    int read = 0;
    int firstError = 0;
    while (first.size() < buffer.size()) {
        read = recv(client->socket, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size() - first.size()), 0);
        if (read <= 0) {
            if (read == SOCKET_ERROR) firstError = WSAGetLastError();
            break;
        }
        first.append(reinterpret_cast<char*>(buffer.data()), static_cast<size_t>(read));
        if (first.find('\n') != std::string::npos) break;
        const std::string protocolV1 = "SERIALCTL/1 ";
        const std::string protocolV2 = "SERIALCTL/2 ";
        const bool matchesV1 = protocolV1.compare(0, std::min(first.size(), protocolV1.size()),
            first, 0, std::min(first.size(), protocolV1.size())) == 0;
        const bool matchesV2 = protocolV2.compare(0, std::min(first.size(), protocolV2.size()),
            first, 0, std::min(first.size(), protocolV2.size())) == 0;
        if (!matchesV1 && !matchesV2) break;
    }
    if (!first.empty()) {
        if (first == "SERIALCTL/1 LIST\n") {
            const std::string response = "SERIALCTL/1 PORTS\n" +
                WideToMultiByte(settings_.portName, CP_UTF8) + "\n.\n";
            send(client->socket, response.data(), static_cast<int>(response.size()), 0);
            read = 0;
        } else if (first.rfind("SERIALCTL/2 OPEN ", 0) == 0) {
            const size_t lineEnd = first.find('\n');
            std::string requested = first.substr(17, lineEnd == std::string::npos ? std::string::npos : lineEnd - 17);
            if (!requested.empty() && requested.back() == '\r') requested.pop_back();
            if (requested != WideToMultiByte(settings_.portName, CP_UTF8)) {
                const std::string response = "SERIALCTL/2 ERR NOT_FOUND\n";
                send(client->socket, response.data(), static_cast<int>(response.size()), 0);
                read = 0;
            } else {
                client->protocol = ClientProtocol::Version2;
                const std::string response = "SERIALCTL/2 OK WRITE\n";
                send(client->socket, response.data(), static_cast<int>(response.size()), 0);
                client->ready = true;
            }
        } else if (first.rfind("SERIALCTL/1 OPEN ", 0) == 0) {
            const size_t lineEnd = first.find('\n');
            std::string requested = first.substr(17, lineEnd == std::string::npos ? std::string::npos : lineEnd - 17);
            if (!requested.empty() && requested.back() == '\r') requested.pop_back();
            if (requested != WideToMultiByte(settings_.portName, CP_UTF8)) {
                const std::string response = "SERIALCTL/1 ERR NOT_FOUND\n";
                send(client->socket, response.data(), static_cast<int>(response.size()), 0);
                read = 0;
            } else {
                const std::string response = "SERIALCTL/1 OK\n";
                send(client->socket, response.data(), static_cast<int>(response.size()), 0);
                client->protocol = ClientProtocol::Version1;
                client->ready = true;
                if (lineEnd != std::string::npos && lineEnd + 1 < first.size()) {
                    Bytes tail(first.begin() + static_cast<std::ptrdiff_t>(lineEnd + 1), first.end());
                    std::wstring error;
                    if (!device_.Write(tail, error) && onStatus_) onStatus_(error, true);
                }
            }
        } else {
            // Existing tools such as gensio speak raw TCP. Preserve that behavior.
            client->protocol = ClientProtocol::Raw;
            client->ready = true;
            Bytes data(first.begin(), first.end());
            std::wstring error;
            if (!device_.Write(data, error) && onStatus_) onStatus_(error, true);
        }
    } else if (read == SOCKET_ERROR && firstError == WSAETIMEDOUT) {
        // A receive-only raw TCP client may send nothing before serial data arrives.
        client->protocol = ClientProtocol::Raw;
        client->ready = true;
    }
    DWORD noTimeout = 0;
    setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));

    if (!client->ready) {
        if (client->socket != INVALID_SOCKET) {
            shutdown(client->socket, SD_BOTH);
            closesocket(client->socket);
            client->socket = INVALID_SOCKET;
        }
        RemoveClient(client);
        return;
    }
    if (onStatus_) onStatus_(L"远程客户端已连接，当前 " + std::to_wstring(ClientCount()) + L" 个 · 均可读写", false);

    while (!stopping_) {
        if (client->protocol == ClientProtocol::Version2) {
            std::array<std::uint8_t, 5> header{};
            if (!ReceiveExact(client->socket, header.data(), header.size())) break;
            const std::uint32_t length = (static_cast<std::uint32_t>(header[1]) << 24) |
                (static_cast<std::uint32_t>(header[2]) << 16) |
                (static_cast<std::uint32_t>(header[3]) << 8) | static_cast<std::uint32_t>(header[4]);
            if (length > 1024 * 1024) break;
            Bytes payload(length);
            if (length && !ReceiveExact(client->socket, payload.data(), payload.size())) break;
            if (header[0] == 'D') {
                std::wstring error;
                if (!device_.Write(payload, error)) {
                    if (onStatus_) onStatus_(error, true);
                    break;
                }
            } else if (header[0] == 'C') {
                const std::string command(payload.begin(), payload.end());
                if (command == "ACQUIRE" || command == "RELEASE") SendClientControl(client, "GRANTED");
            }
        } else {
            read = recv(client->socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (read <= 0) break;
            Bytes data(buffer.begin(), buffer.begin() + read);
            std::wstring error;
            if (!device_.Write(data, error)) {
                if (onStatus_) onStatus_(error, true);
                break;
            }
        }
    }

    if (client->socket != INVALID_SOCKET) {
        shutdown(client->socket, SD_BOTH);
        closesocket(client->socket);
        client->socket = INVALID_SOCKET;
    }
    RemoveClient(client);
    if (!stopping_ && onStatus_) {
        onStatus_(L"远程客户端已断开，当前 " + std::to_wstring(ClientCount()) + L" 个", false);
    }
}

void SerialShareConnection::Broadcast(const Bytes& data) {
    std::vector<std::shared_ptr<Client>> snapshot;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        snapshot = clients_;
    }
    for (const auto& client : snapshot) {
        if (!client->ready) continue;
        std::lock_guard<std::mutex> sendLock(client->sendMutex);
        if (client->protocol == ClientProtocol::Version2) {
            SendFrame(client->socket, 'D', data);
            continue;
        }
        size_t offset = 0;
        while (offset < data.size() && client->socket != INVALID_SOCKET) {
            const int sent = send(
                client->socket,
                reinterpret_cast<const char*>(data.data() + offset),
                static_cast<int>(data.size() - offset),
                0);
            if (sent == SOCKET_ERROR) {
                break;
            }
            offset += static_cast<size_t>(sent);
        }
    }
}

void SerialShareConnection::RemoveClient(const std::shared_ptr<Client>& client) {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
}

size_t SerialShareConnection::ClientCount() {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    return clients_.size();
}

bool SerialShareConnection::SendClientControl(const std::shared_ptr<Client>& client, const std::string& message) {
    std::lock_guard<std::mutex> sendLock(client->sendMutex);
    return SendFrame(client->socket, 'C', Bytes(message.begin(), message.end()));
}

} // namespace serialctl
