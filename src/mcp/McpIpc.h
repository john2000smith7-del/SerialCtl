#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <atomic>
#include <thread>

namespace serialctl::mcp {

constexpr const wchar_t* DefaultMcpPipeName = L"\\\\.\\pipe\\SerialCtl.McpBridge.v1";

// Client helper: sends request to pipe server and reads response
bool SendIpcRequest(const std::wstring& pipeName, const std::string& request,
                    std::string& response, DWORD timeoutMs = 3000);

// Server: listens on Named Pipe, dispatches incoming requests to Handler
class McpIpcServer {
public:
    using Handler = std::function<std::string(const std::string& request)>;

    explicit McpIpcServer(std::wstring pipeName = DefaultMcpPipeName);
    ~McpIpcServer();

    bool Start(Handler handler);
    void Stop();
    bool IsRunning() const { return running_.load(); }
    const std::wstring& PipeName() const { return pipeName_; }

private:
    void RunServer(HANDLE nextPipe, PSECURITY_DESCRIPTOR descriptor);

    std::wstring pipeName_;
    Handler handler_;
    std::atomic<bool> running_{false};
    HANDLE stopEvent_ = nullptr;
    std::thread workerThread_;
};

} // namespace serialctl::mcp
