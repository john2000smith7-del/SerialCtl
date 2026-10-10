#pragma once
#include "Connection.h"
#include "GatewayClient.h"
#include <atomic>
#include <thread>
namespace serialctl {
class SharedSerialConnection final : public IConnection {
  public:
    SharedSerialConnection(std::wstring host, std::uint16_t port, std::wstring serialName)
        : host_(std::move(host)), port_(port), serialName_(std::move(serialName)) {}
    ~SharedSerialConnection() override {
        Stop();
    }
    static bool Discover(const std::wstring &, std::uint16_t, std::vector<std::wstring> &, std::wstring &,
                         std::vector<std::wstring> *descriptions = nullptr);
    static bool DiscoverAuto(const std::wstring &, std::uint16_t &, std::vector<std::wstring> &, std::wstring &,
                             const std::atomic_bool *cancel = nullptr, std::uint16_t explicitPort = 0,
                             std::vector<std::wstring> *descriptions = nullptr);
    bool Start(DataCallback, StatusCallback, std::wstring &) override;
    void CancelStart() override {
        cancel_ = true;
        client_.Cancel();
    }
    void Stop() override;
    bool Send(const Bytes &, std::wstring &) override;
    bool IsConnected() const override {
        return connected_;
    }

  private:
    void Read();
    std::wstring host_, serialName_;
    std::uint16_t port_;
    std::string resource_;
    GatewayClient client_;
    std::atomic_bool cancel_{false}, connected_{false}, stopping_{false};
    std::thread reader_;
    DataCallback data_;
    StatusCallback status_;
};
} // namespace serialctl
