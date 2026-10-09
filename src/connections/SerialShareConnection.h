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

namespace serialctl {

class SerialShareConnection final : public IConnection {
public:
    SerialShareConnection(SerialSettings settings, std::uint16_t listenPort);
    ~SerialShareConnection() override;

    bool Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) override;
    void Stop() override;
    bool Send(const Bytes& data, std::wstring& error) override;
    bool IsConnected() const override;

private:
    enum class ClientProtocol { Raw, Version1, Version2 };
    struct Client {
        SOCKET socket = INVALID_SOCKET;
        std::atomic<bool> ready{false};
        ClientProtocol protocol = ClientProtocol::Raw;
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
    void Broadcast(const Bytes& data);
    void SendLoop(const std::shared_ptr<Client>& client);
    bool QueueSend(const std::shared_ptr<Client>& client, char type, const Bytes& data);
    void ShutdownClient(const std::shared_ptr<Client>& client);
    void FinishClient(const std::shared_ptr<Client>& client);
    void RemoveClient(const std::shared_ptr<Client>& client);
    size_t ClientCount();
    bool SendClientControl(const std::shared_ptr<Client>& client, const std::string& message);

    SerialSettings settings_;
    std::uint16_t listenPort_;
    SerialDevice device_;
    std::atomic<SOCKET> listener_{INVALID_SOCKET};
    std::atomic<bool> stopping_{false};
    std::thread acceptThread_;
    std::mutex clientsMutex_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::vector<std::pair<std::shared_ptr<Client>, std::thread>> clientThreads_;
    DataCallback onData_;
    StatusCallback onStatus_;
};

} // namespace serialctl
