#pragma once

#include "Connection.h"

#include <atomic>
#include <mutex>
#include <thread>

#include <winsock2.h>

namespace serialctl {

class TcpConnection final : public IConnection {
public:
    TcpConnection(std::wstring host, std::uint16_t port, bool telnet);
    ~TcpConnection() override;

    bool Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) override;
    void CancelStart() override { cancelStarting_ = true; }
    void Stop() override;
    bool Send(const Bytes& data, std::wstring& error) override;
    bool IsConnected() const override;
    void ResizeTerminal(int columns, int rows) override;

private:
    void ReadLoop();
    Bytes DecodeTelnet(const std::uint8_t* data, size_t size, Bytes& reply);
    bool SendWire(const Bytes& data, std::wstring& error);
    void AppendWindowSize(Bytes& output) const;

    std::wstring host_;
    std::uint16_t port_;
    bool telnet_;
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::atomic_bool connected_{false};
    std::atomic_bool cancelStarting_{false};
    std::atomic<bool> stopping_{false};
    std::thread readThread_;
    std::mutex sendMutex_;
    DataCallback onData_;
    StatusCallback onStatus_;
    int telnetState_ = 0;
    std::uint8_t telnetCommand_ = 0;
    std::uint8_t telnetSubnegotiationOption_ = 0;
    std::atomic<bool> telnetWindowSizeEnabled_{false};
    std::atomic<int> terminalColumns_{80};
    std::atomic<int> terminalRows_{24};
};

} // namespace serialctl
