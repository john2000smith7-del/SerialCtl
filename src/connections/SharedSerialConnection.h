#pragma once

#include "Connection.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace serialctl {

class SharedSerialConnection final : public IConnection {
public:
    SharedSerialConnection(std::wstring host, std::uint16_t port, std::wstring serialName);
    ~SharedSerialConnection() override;

    static bool Discover(
        const std::wstring& host,
        std::uint16_t port,
        std::vector<std::wstring>& serialNames,
        std::wstring& error);

    bool Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) override;
    void CancelStart() override { cancelStarting_ = true; }
    void Stop() override;
    bool Send(const Bytes& data, std::wstring& error) override;
    bool IsConnected() const override;

private:
    void ReadLoop();
    bool SendFrame(char type, const Bytes& payload, std::wstring& error);

    std::wstring host_;
    std::uint16_t port_;
    std::wstring serialName_;
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::atomic_bool cancelStarting_{false};
    std::atomic<bool> stopping_{false};
    std::thread readThread_;
    std::mutex sendMutex_;
    Bytes receivedBuffer_;
    DataCallback onData_;
    StatusCallback onStatus_;
};

} // namespace serialctl
