#pragma once
#include "Connection.h"
#include "GatewayClient.h"
#include <atomic>
#include <thread>
#include <condition_variable>
#include <map>
#include <set>
namespace serialctl {
class SharedSerialConnection final : public IConnection {
  public:
    SharedSerialConnection(std::wstring host, std::uint16_t port, std::wstring serialName, std::string instance = {})
        : host_(std::move(host)), port_(port), serialName_(std::move(serialName)),
          expectedInstance_(std::move(instance)) {}
    ~SharedSerialConnection() override {
        Stop();
    }
    static bool Discover(const std::wstring &, std::uint16_t, std::vector<std::wstring> &, std::wstring &,
                         std::vector<std::wstring> *descriptions = nullptr);
    static bool DiscoverAuto(const std::wstring &, std::uint16_t &, std::vector<std::wstring> &, std::wstring &,
                             const std::atomic_bool *cancel = nullptr, std::uint16_t explicitPort = 0,
                             std::vector<std::wstring> *descriptions = nullptr, std::string *instance = nullptr);
    bool Start(DataCallback, StatusCallback, std::wstring &) override;
    void CancelStart() override {
        cancel_ = true;
        client_.Cancel();
    }
    void Stop() override;
    bool Send(const Bytes &, std::wstring &) override;
    bool SupportsTransfer() const override {return transferSupported_;}
    bool BeginTransferWire(const std::string&,std::wstring&) override;
    bool SendTransferWire(const std::string&,const Bytes&,std::wstring&) override;
    bool EndTransferWire(const std::string&,std::wstring&) override;
    bool IsConnected() const override {
        return connected_;
    }

  private:
    void Read();
    bool TransferRequest(const std::string&,nlohmann::json,std::wstring&,nlohmann::json* result=nullptr);
    std::wstring host_, serialName_;
    std::uint16_t port_;
    std::string resource_, expectedInstance_;
    GatewayClient client_;
    std::atomic_bool cancel_{false}, connected_{false}, stopping_{false};
    std::atomic_bool transferSupported_{false};
    std::mutex rpcMutex_;
    std::condition_variable rpcWake_;
    std::set<std::string> pendingRpc_;
    std::map<std::string,nlohmann::json> rpcReplies_;
    std::string transferToken_;
    std::thread reader_;
    DataCallback data_;
    StatusCallback status_;
};
} // namespace serialctl
