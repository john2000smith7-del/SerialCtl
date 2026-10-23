#pragma once
#include "Connection.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace serialctl {
// Optional raw datagrams, not serialctl.v1 discovery. A bound socket means
// ready to receive; it does not prove a peer or a physical serial port exists.
class UdpSerialConnection final : public IConnection {
  public:
    UdpSerialConnection(std::wstring host, std::uint16_t peerPort = 6333, std::uint16_t localPort = 6333)
        : host_(std::move(host)), peerPort_(peerPort), localPort_(localPort) {}
    ~UdpSerialConnection() override {
        Stop();
    }
    bool Start(DataCallback data, StatusCallback status, std::wstring &error) override;
    void CancelStart() override {
        cancel_ = true;
    }
    void Stop() override;
    bool Send(const Bytes &, std::wstring &error) override;
    bool IsConnected() const override {
        return socket_ != INVALID_SOCKET && !stopping_;
    }
    std::uint16_t LocalPort() const {
        return boundPort_;
    }

  private:
    void Read();
    std::wstring host_;
    std::uint16_t peerPort_, localPort_, boundPort_ = 0;
    sockaddr_storage peer_{};
    int peerLength_ = 0;
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::atomic_bool stopping_{true}, cancel_{false};
    std::mutex sendMutex_;
    std::thread reader_;
    DataCallback data_;
    StatusCallback status_;
};
} // namespace serialctl
