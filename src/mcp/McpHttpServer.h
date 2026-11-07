#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace serialctl::mcp {

class McpHttpServer {
public:
    using RequestHandler = std::function<std::string(const std::string& clientIp, const std::string& requestBody)>;
    using HealthHandler = std::function<std::string()>;

    McpHttpServer();
    ~McpHttpServer();

    bool Start(const std::string& bindIp, uint16_t port,
               RequestHandler requestHandler, HealthHandler healthHandler,
               std::wstring& error);
    void Stop();

    bool IsRunning() const { return running_.load(); }
    std::string BindIp() const { return bindIp_; }
    uint16_t Port() const { return port_; }

private:
    void AcceptLoop();
    void HandleClient(SOCKET clientSocket, std::string clientIp);

    std::atomic<bool> running_{false};
    SOCKET listenSocket_ = INVALID_SOCKET;
    std::string bindIp_ = "0.0.0.0";
    uint16_t port_ = 7080;
    RequestHandler requestHandler_;
    HealthHandler healthHandler_;
    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::thread acceptThread_;
    std::mutex clientThreadsMutex_;
    std::vector<ClientThread> clientThreads_;
    static constexpr size_t kMaxConcurrentHttpClients = 32;
};

} // namespace serialctl::mcp
