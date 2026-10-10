#include "SerialShareConnection.h"
#include "../network/DiscoveryInfo.h"
#include "Win32Helpers.h"

#include <algorithm>
#include <cstdio>
#ifdef SERIALCTL_TEST_TRACE
#define SHARE_TRACE(text) std::fprintf(stderr, "share[%lu] %s\n", GetCurrentThreadId(), text)
#else
#define SHARE_TRACE(text) ((void)0)
#endif
#include <array>
#include <string>
#include <cwctype>

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

int ReceiveCancelable(SOCKET socket, std::uint8_t* data, size_t size,
    const std::atomic<bool>& stopping, const std::atomic<bool>& closing) {
    while (!stopping && !closing) {
        const int received = recv(socket, reinterpret_cast<char*>(data), static_cast<int>(size), 0);
        if (received != SOCKET_ERROR || WSAGetLastError() != WSAETIMEDOUT) return received;
    }
    return 0;
}

bool ReceiveExact(SOCKET socket, Bytes& buffered, std::uint8_t* data, size_t size,
    const std::atomic<bool>& stopping, const std::atomic<bool>& closing) {
    const size_t bufferedCount = std::min(size, buffered.size());
    std::copy_n(buffered.begin(), bufferedCount, data);
    buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(bufferedCount));
    size_t offset = bufferedCount;
    while (offset < size) {
        const int received = ReceiveCancelable(socket, data + offset, size - offset, stopping, closing);
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

namespace {
std::wstring NormalizeSerial(std::wstring name) {
    if (name.rfind(L"\\\\.\\", 0) == 0) name.erase(0, 4);
    std::transform(name.begin(), name.end(), name.begin(), ::towupper);
    return name;
}
}
namespace { std::mutex sharedServiceMutex; std::weak_ptr<SerialShareService> sharedService; std::atomic_bool legacyEnabled{true}; }
bool SerialShareService::LegacyEnabled() { return legacyEnabled; }
bool SerialShareService::SetLegacyEnabled(bool enabled,std::wstring& error) {
    legacyEnabled=enabled;std::shared_ptr<SerialShareService> service;
    {std::lock_guard<std::mutex> lock(sharedServiceMutex);service=sharedService.lock();}
    if(!service)return true;
    if(!enabled) {service->Stop();return true;}
    return service->Start(error);
}
std::shared_ptr<SerialShareService> SerialShareService::Acquire(std::uint16_t port, std::wstring& error) {
    std::lock_guard<std::mutex> lock(sharedServiceMutex);
    if (auto service = sharedService.lock()) return service;
    auto service = std::make_shared<SerialShareService>(port);
    if (legacyEnabled && !service->Start(error)) return {};
    sharedService = service;
    return service;
}

bool SerialShareService::Start(std::wstring& error) {
    if(listener_ != INVALID_SOCKET)return true;
    const unsigned preferred = listenPort_;
    for (unsigned port = preferred; port <= 65535 && port < preferred + 16; ++port) {
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) continue;
        BOOL exclusive = TRUE;
        if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
            closesocket(listener); continue;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<u_short>(port));
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(listener, SOMAXCONN) == SOCKET_ERROR) { closesocket(listener); continue; }
        u_long nonBlocking = 1;
        if (ioctlsocket(listener, FIONBIO, &nonBlocking) == SOCKET_ERROR) { closesocket(listener); continue; }
        int length = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        listenPort_ = ntohs(address.sin_port);
        listener_ = listener;
        stopping_ = false;
        acceptThread_ = std::thread(&SerialShareService::AcceptLoop, this);
        return true;
    }
    error = L"共享端口不可用，本地串口仍可使用（默认自动尝试 7000–7015）。";
    return false;
}

bool SerialShareConnection::Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) {
    if (channel_) { error = L"串口已经打开"; return false; }
    std::wstring shareError;
    auto service = SerialShareService::Acquire(listenPort_, shareError);
    auto channel = std::make_shared<SerialShareChannel>();
    channel->settings = settings_;
    channel->onStatus = onStatus;
    std::weak_ptr<SerialShareService> weakService = service;
    std::weak_ptr<SerialShareChannel> weakChannel = channel;
    if (!channel->device.Open(settings_, [onData, weakService, weakChannel](const Bytes& data) {
            if (onData) onData(data);
            if (auto current = weakChannel.lock())
                if (auto gateway = weakService.lock()) gateway->Broadcast(current, data);
        }, onStatus, error)) return false;
    channel->active = true;
    if (service && !service->Register(channel)) {
        channel->active = false;
        channel->device.Close();
        error = L"该串口已经打开";
        return false;
    }
    channel_ = channel;
    service_ = service;
    if (onStatus) {
        if (service && service->Port()) onStatus(L"正在共享 " + settings_.portName + L"，TCP 端口 " + std::to_wstring(service->Port()) + L" · 各串口独立 · 均可读写", false);
        else if (service) onStatus(L"串口本地连接成功，兼容 TCP 服务已关闭", false);
        else onStatus(shareError, true);
    }
    return true;
}
void SerialShareConnection::Stop() {
    if (!channel_) return;
    channel_->active = false;
    if (service_) service_->Unregister(channel_);
    { std::lock_guard<std::mutex> lock(channel_->ioMutex); channel_->device.Close(); }
    channel_.reset();
    service_.reset();
}
bool SerialShareConnection::Send(const Bytes& data, std::wstring& error) {
    if (!channel_) { error = L"串口尚未打开"; return false; }
    return channel_->Write(data, error);
}
bool SerialShareConnection::IsConnected() const { return channel_ && channel_->active && channel_->device.IsOpen(); }

bool SerialShareService::Register(const std::shared_ptr<SerialShareChannel>& channel) {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    return channels_.emplace(NormalizeSerial(channel->settings.portName), channel).second;
}
bool SerialShareService::SelectClient(const std::shared_ptr<Client>& client, const std::wstring& name, ClientProtocol protocol) {
    // Publish the binding before OPEN is acknowledged. Unregister uses this same
    // map lock, so a concurrent COM close cannot miss a half-established client.
    std::lock_guard<std::mutex> lock(channelsMutex_);
    auto found = name.empty() ? (channels_.size() == 1 ? channels_.begin() : channels_.end()) : channels_.find(NormalizeSerial(name));
    if (found == channels_.end() || !found->second->active) return false;
    client->channel = found->second;
    client->protocol = protocol;
    client->ready = true;
    return true;
}
void SerialShareService::Unregister(const std::shared_ptr<SerialShareChannel>& channel) {
    { std::lock_guard<std::mutex> lock(channelsMutex_); channels_.erase(NormalizeSerial(channel->settings.portName)); }
    std::vector<std::shared_ptr<Client>> snapshot;
    { std::lock_guard<std::mutex> lock(clientsMutex_); snapshot = clients_; }
    for (const auto& client : snapshot) if (client->ready && client->channel == channel) ShutdownClient(client);
}
std::string SerialShareService::ListReply() {
    std::vector<std::shared_ptr<SerialShareChannel>> channels;
    { std::lock_guard<std::mutex> lock(channelsMutex_); for (const auto& entry : channels_) if (entry.second->active) channels.push_back(entry.second); }
    std::vector<std::shared_ptr<Client>> clients;
    { std::lock_guard<std::mutex> lock(clientsMutex_); clients = clients_; }
    std::string reply = "SERIALCTL/1 PORTS\n";
    for (const auto& channel : channels) reply += WideToMultiByte(NormalizeSerial(channel->settings.portName), CP_UTF8) + "\n";
    reply += ".\nSERIALCTL_INFO\t3\n";
    for (const auto& channel : channels) {
        size_t count = 0;
        for (const auto& client : clients) if (client->ready && client->channel == channel) ++count;
        const auto& s = channel->settings;
        reply += WideToMultiByte(NormalizeSerial(s.portName), CP_UTF8) + "\t" + std::to_string(s.baudRate) + "\t" +
            std::to_string(s.dataBits) + "\t" + std::to_string(s.parity) + "\t" + std::to_string(s.stopBits) + "\t" +
            std::to_string(s.flowControl) + "\t" + std::to_string(count) + "\topen\n";
    }
    return reply;
}

void SerialShareService::Stop() {
    stopping_ = true;
    SOCKET listener = listener_.exchange(INVALID_SOCKET);
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (acceptThread_.joinable()) acceptThread_.join();
    std::vector<std::shared_ptr<Client>> clients;
    { std::lock_guard<std::mutex> lock(clientsMutex_); clients = clients_; }
    for (const auto& client : clients) ShutdownClient(client);
    for (auto& worker : clientThreads_) if (worker.second.joinable()) worker.second.join();
    clientThreads_.clear();
    { std::lock_guard<std::mutex> lock(clientsMutex_); clients_.clear(); }
}

void SerialShareService::AcceptLoop() {
    while (!stopping_) {
        SOCKET accepted = accept(listener_, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            const int acceptError = WSAGetLastError();
            if (!stopping_ && acceptError == WSAEWOULDBLOCK) {
                SOCKET listener = listener_.load();
                if (listener == INVALID_SOCKET) break;
                fd_set readable{};
                FD_ZERO(&readable);
                FD_SET(listener, &readable);
                timeval interval{0, 100000};
                select(0, &readable, nullptr, nullptr, &interval);
                continue;
            }

            break;
        }
        if (stopping_) { closesocket(accepted); break; }
        u_long blocking = 0;
        if (ioctlsocket(accepted, FIONBIO, &blocking) == SOCKET_ERROR) {
            closesocket(accepted);
            continue;
        }
        DWORD sendTimeout = 500;
        setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
        BOOL noDelay = TRUE;
        setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        auto client = std::make_shared<Client>();
        client->socket = accepted;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (clients_.size() >= 128) {
                closesocket(accepted);
                continue;
            }
            for (auto worker = clientThreads_.begin(); worker != clientThreads_.end();) {
                if (worker->first->finished.load()) {
                    if (worker->second.joinable()) worker->second.join();
                    worker = clientThreads_.erase(worker);
                } else ++worker;
            }
            clients_.push_back(client);
            clientThreads_.emplace_back(client, std::thread(&SerialShareService::ClientLoop, this, client));
        }
    }
}

void SerialShareService::ClientLoop(const std::shared_ptr<Client>& client) {
    std::array<std::uint8_t, 4096> buffer{};
    DWORD handshakeTimeout = 400;
    setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&handshakeTimeout), sizeof(handshakeTimeout));
    std::string first;
    Bytes buffered;
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
        const std::string protocolV3 = "SERIALCTL/3 ";
        const bool matchesV3 = protocolV3.compare(0, std::min(first.size(), protocolV3.size()), first, 0, std::min(first.size(), protocolV3.size())) == 0;
        const bool matchesV1 = protocolV1.compare(0, std::min(first.size(), protocolV1.size()),
            first, 0, std::min(first.size(), protocolV1.size())) == 0;
        const bool matchesV2 = protocolV2.compare(0, std::min(first.size(), protocolV2.size()),
            first, 0, std::min(first.size(), protocolV2.size())) == 0;
        if (!matchesV1 && !matchesV2 && !matchesV3) break;
    }
    const size_t lineEnd = first.find('\n');
    const std::string line = lineEnd == std::string::npos ? first : first.substr(0, lineEnd + 1);
    if (line == "SERIALCTL/3 DISCOVER\n") {
        const auto reply=ApiDiscoveryReply(); SendAll(client->socket, reinterpret_cast<const std::uint8_t*>(reply.data()), reply.size());
    } else if (line == "SERIALCTL/1 LIST\n") {
        const auto reply = ListReply();
        SendAll(client->socket, reinterpret_cast<const std::uint8_t*>(reply.data()), reply.size());
    } else if (first.rfind("SERIALCTL/2 OPEN ", 0) == 0 || first.rfind("SERIALCTL/1 OPEN ", 0) == 0) {
        const bool version2 = first[10] == '2';
        std::string requested = first.substr(17, lineEnd == std::string::npos ? std::string::npos : lineEnd - 17);
        if (!requested.empty() && requested.back() == '\r') requested.pop_back();
        const bool selected = lineEnd != std::string::npos && SelectClient(client, MultiByteToWide(
            reinterpret_cast<const std::uint8_t*>(requested.data()), requested.size(), CP_UTF8), version2 ? ClientProtocol::Version2 : ClientProtocol::Version1);
        const std::string prefix = version2 ? "SERIALCTL/2 " : "SERIALCTL/1 ";
        const std::string response = prefix + (selected ? (version2 ? "OK WRITE\n" : "OK\n") : "ERR NOT_FOUND\n");
        const bool replied = SendAll(client->socket, reinterpret_cast<const std::uint8_t*>(response.data()), response.size());
        if (!replied) client->ready = false;
        if (replied && selected) {
            if (lineEnd + 1 < first.size()) buffered.assign(first.begin() + static_cast<std::ptrdiff_t>(lineEnd + 1), first.end());
            if (!version2 && !buffered.empty()) { std::wstring error; if (!client->channel->Write(buffered, error)) client->ready = false; buffered.clear(); }
        }
    } else if (first.rfind("SERIALCTL/", 0) == 0) {
        const std::string response = "SERIALCTL/2 ERR UNSUPPORTED\n";
        SendAll(client->socket, reinterpret_cast<const std::uint8_t*>(response.data()), response.size());
    } else if (!first.empty() || (read == SOCKET_ERROR && firstError == WSAETIMEDOUT)) {
        // Raw clients are safe only when a single COM is shared. Named OPEN is mandatory otherwise.
        if (SelectClient(client, L"", ClientProtocol::Raw)) {
            if (!first.empty()) { std::wstring error; if (!client->channel->Write(Bytes(first.begin(), first.end()), error)) client->ready = false; }
        } else {
            const std::string response = "SERIALCTL/2 ERR SELECT_REQUIRED\n";
            SendAll(client->socket, reinterpret_cast<const std::uint8_t*>(response.data()), response.size());
        }
    }
    // Windows shutdown does not reliably wake another thread's blocking recv.
    // A short receive timeout lets cancellation be observed even for idle peers.
    DWORD receiveTimeout = 200;
    setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));

    if (!client->ready || stopping_ || client->closing || !client->channel || !client->channel->active) {
        FinishClient(client);
        return;
    }
    try {
        client->sendThread = std::thread(&SerialShareService::SendLoop, this, client);
    } catch (...) {
        FinishClient(client);
        return;
    }


    if (client->channel->onStatus) client->channel->onStatus(L"远程客户端已连接 · " + client->channel->settings.portName + L" · 均可读写", false);
    while (!stopping_ && !client->closing) {
        if (client->protocol == ClientProtocol::Version2) {
            std::array<std::uint8_t, 5> header{};
            if (!ReceiveExact(client->socket, buffered, header.data(), header.size(), stopping_, client->closing)) break;
            const std::uint32_t length = (static_cast<std::uint32_t>(header[1]) << 24) |
                (static_cast<std::uint32_t>(header[2]) << 16) |
                (static_cast<std::uint32_t>(header[3]) << 8) | static_cast<std::uint32_t>(header[4]);
            if (length > 1024 * 1024) break;
            Bytes payload(length);
            if (length && !ReceiveExact(client->socket, buffered, payload.data(), payload.size(), stopping_, client->closing)) break;
            if (header[0] == 'D') {
                std::wstring error;
                if (!client->channel->Write(payload, error)) {
                    break;
                }
            } else if (header[0] == 'C') {
                const std::string command(payload.begin(), payload.end());
                if (command == "ACQUIRE" || command == "RELEASE") SendClientControl(client, "GRANTED");
            }
        } else {
            read = ReceiveCancelable(client->socket, buffer.data(), buffer.size(), stopping_, client->closing);
            if (read <= 0) break;
            Bytes data(buffer.begin(), buffer.begin() + read);
            std::wstring error;
            if (!client->channel->Write(data, error)) {
                break;
            }
        }
    }

    FinishClient(client);
}

void SerialShareService::ShutdownClient(const std::shared_ptr<Client>& client) {
    // Change the wait predicate under the same mutex as condition_variable::wait
    // so notification cannot be lost between its predicate check and sleeping.
    {
        std::lock_guard<std::mutex> queueLock(client->queueMutex);
        client->closing.store(true);
    }
    SHARE_TRACE("notify sender");
    client->queueReady.notify_all();
    std::lock_guard<std::mutex> lock(client->lifecycleMutex);
    SHARE_TRACE("shutdown socket begin");
    if (client->socket != INVALID_SOCKET) shutdown(client->socket, SD_BOTH);
    SHARE_TRACE("shutdown socket end");
}

void SerialShareService::FinishClient(const std::shared_ptr<Client>& client) {
    client->ready.store(false);
    ShutdownClient(client);
    SHARE_TRACE("joining sender");
    if (client->sendThread.joinable()) client->sendThread.join();
    SHARE_TRACE("sender joined");
    {
        std::lock_guard<std::mutex> lock(client->lifecycleMutex);
        if (client->socket != INVALID_SOCKET) {
            closesocket(client->socket);
            client->socket = INVALID_SOCKET;
        }
    }
    RemoveClient(client);

    client->finished.store(true);
    SHARE_TRACE("client finished");
}

bool SerialShareService::QueueSend(const std::shared_ptr<Client>& client, char type, const Bytes& data) {
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(client->queueMutex);
        if (client->closing) return false;
        constexpr size_t QueueLimit = 1024 * 1024;
        overflow = client->queuedBytes + data.size() + 5 > QueueLimit;
        if (!overflow) {
            client->queue.emplace_back(type, data);
            client->queuedBytes += data.size() + 5;
        }
    }
    if (overflow) {
        ShutdownClient(client);
        return false;
    }
    client->queueReady.notify_one();
    return true;
}

void SerialShareService::SendLoop(const std::shared_ptr<Client>& client) {
    SHARE_TRACE("sender started");
    while (!stopping_ && !client->closing) {
        std::pair<char, Bytes> frame;
        {
            std::unique_lock<std::mutex> lock(client->queueMutex);
            client->queueReady.wait(lock, [&] { return stopping_ || client->closing || !client->queue.empty(); });
            if (stopping_ || client->closing) break;
            frame = std::move(client->queue.front());
            client->queue.pop_front();
            client->queuedBytes -= frame.second.size() + 5;
        }
        const bool ok = client->protocol == ClientProtocol::Version2 ?
            SendFrame(client->socket, frame.first, frame.second) :
            SendAll(client->socket, frame.second.data(), frame.second.size());
        if (!ok) { ShutdownClient(client); break; }
    }
    SHARE_TRACE("sender finished");
}

void SerialShareService::Broadcast(const std::shared_ptr<SerialShareChannel>& channel, const Bytes& data) {
    std::vector<std::shared_ptr<Client>> snapshot;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        snapshot = clients_;
    }
    for (const auto& client : snapshot) {
        if (client->ready && client->channel == channel) QueueSend(client, 'D', data);
    }
}

void SerialShareService::RemoveClient(const std::shared_ptr<Client>& client) {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
}

size_t SerialShareService::ClientCount() {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    return clients_.size();
}

bool SerialShareService::SendClientControl(const std::shared_ptr<Client>& client, const std::string& message) {
    return QueueSend(client, 'C', Bytes(message.begin(), message.end()));
}

} // namespace serialctl
