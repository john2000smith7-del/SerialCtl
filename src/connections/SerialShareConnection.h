#pragma once

#include "Connection.h"
#include "SerialDevice.h"

#include <atomic>
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
        std::mutex sendMutex;
    };

    void AcceptLoop();
    void ClientLoop(const std::shared_ptr<Client>& client);
    void Broadcast(const Bytes& data);
    void RemoveClient(const std::shared_ptr<Client>& client);
    size_t ClientCount();
    bool SendClientControl(const std::shared_ptr<Client>& client, const std::string& message);

    SerialSettings settings_;
    std::uint16_t listenPort_;
    SerialDevice device_;
    SOCKET listener_ = INVALID_SOCKET;
    std::atomic<bool> stopping_{false};
    std::thread acceptThread_;
    std::mutex clientsMutex_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::vector<std::thread> clientThreads_;
    DataCallback onData_;
    StatusCallback onStatus_;
};

} // namespace serialctl
