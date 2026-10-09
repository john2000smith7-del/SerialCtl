#pragma once

#include "Connection.h"
#include "SerialDevice.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <map>

namespace serialctl {

struct SerialShareChannel {
    SerialSettings settings;
    SerialDevice device;
    StatusCallback onStatus;
    std::atomic_bool active{false};
    std::mutex ioMutex;
    bool Write(const Bytes& data, std::wstring& error) {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!active) { error = L"串口已经关闭"; return false; }
        return device.Write(data, error);
    }
};

class SerialShareService {
public:
    explicit SerialShareService(std::uint16_t preferredPort) : listenPort_(preferredPort) {}
    ~SerialShareService() { Stop(); }
    bool Start(std::wstring& error);
    void Stop();
    bool Register(const std::shared_ptr<SerialShareChannel>& channel);
    void Unregister(const std::shared_ptr<SerialShareChannel>& channel);
    void Broadcast(const std::shared_ptr<SerialShareChannel>& channel, const Bytes& data);
    std::uint16_t Port() const { return listenPort_; }
    static std::shared_ptr<SerialShareService> Acquire(std::uint16_t port, std::wstring& error);
private:
    enum class ClientProtocol { Raw, Version1, Version2 };
    struct Client {
        SOCKET socket = INVALID_SOCKET;
        std::atomic<bool> ready{false};
        ClientProtocol protocol = ClientProtocol::Raw;
        std::shared_ptr<SerialShareChannel> channel;
        std::mutex queueMutex;
        std::mutex lifecycleMutex;
        std::condition_variable queueReady;
        std::deque<std::pair<char, Bytes>> queue;
        size_t queuedBytes = 0;
        std::atomic_bool closing{false};
        std::atomic_bool finished{false};
        std::thread sendThread;
    };

    void AcceptLoop();
    void ClientLoop(const std::shared_ptr<Client>& client);
    void SendLoop(const std::shared_ptr<Client>& client);
    bool QueueSend(const std::shared_ptr<Client>& client, char type, const Bytes& data);
    void ShutdownClient(const std::shared_ptr<Client>& client);
    void FinishClient(const std::shared_ptr<Client>& client);
    void RemoveClient(const std::shared_ptr<Client>& client);
    size_t ClientCount();
    bool SendClientControl(const std::shared_ptr<Client>& client, const std::string& message);

    std::uint16_t listenPort_;
    std::mutex channelsMutex_;
    std::map<std::wstring, std::shared_ptr<SerialShareChannel>> channels_;
    std::shared_ptr<SerialShareChannel> FindChannel(const std::wstring& name);
    std::shared_ptr<SerialShareChannel> OnlyChannel();
    std::string ListReply();
    std::atomic<SOCKET> listener_{INVALID_SOCKET};
    std::atomic<bool> stopping_{false};
    std::thread acceptThread_;
    std::mutex clientsMutex_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::vector<std::pair<std::shared_ptr<Client>, std::thread>> clientThreads_;
};

class SerialShareConnection final : public IConnection {
public:
    SerialShareConnection(SerialSettings settings, std::uint16_t port)
        : settings_(std::move(settings)), listenPort_(port) {}
    ~SerialShareConnection() override { Stop(); }
    bool Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) override;
    void Stop() override;
    bool Send(const Bytes& data, std::wstring& error) override;
    bool IsConnected() const override;
    std::uint16_t SharedPort() const { return service_ ? service_->Port() : 0; }
private:
    SerialSettings settings_;
    std::uint16_t listenPort_;
    std::shared_ptr<SerialShareChannel> channel_;
    std::shared_ptr<SerialShareService> service_;
};
} // namespace serialctl
