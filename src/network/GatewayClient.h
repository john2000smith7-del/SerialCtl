#pragma once
#include "WebSocket.h"
#include "json.hpp"
#include <atomic>
#include <memory>
#include <mutex>
namespace serialctl {
// Used by the native remote-serial client and production-protocol tests.
// One receive owner; asynchronous clients serialize sends separately.
class GatewayClient {
  public:
    ~GatewayClient() {
        Close();
    }
    void Connect(const std::wstring &, unsigned, const std::atomic_bool *cancel = nullptr, DWORD timeout = 700);
    void Close();
    void Cancel() {
        auto s = socket_.load();
        if (s != INVALID_SOCKET)
            shutdown(s, SD_BOTH);
    }
    nlohmann::json Request(const std::string &op, const std::string &resource = "",
                           nlohmann::json params = nlohmann::json::object());
    std::string SendRequest(const std::string &op, const std::string &resource, nlohmann::json params);
    nlohmann::json Receive();
    const nlohmann::json &Hello() const {
        return hello_;
    }
    void Live() {
        ws::SocketOptions(socket_);
    }

  private:
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::unique_ptr<ws::Stream> stream_;
    nlohmann::json hello_;
    std::mutex send_;
    std::uint64_t request_ = 0;
};
} // namespace serialctl
