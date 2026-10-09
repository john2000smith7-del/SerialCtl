#pragma once

#include "Connection.h"
#include "PuttyHostKey.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace serialctl {

class PlinkConnection final : public IConnection {
public:
    PlinkConnection(std::wstring host, std::uint16_t port, std::wstring username,
        std::wstring password, int terminalColumns, int terminalRows, HostKeyConfirmation confirm = {});
    ~PlinkConnection() override;

    bool Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) override;
    void CancelStart() override { stopping_ = true; }
    void Stop() override;
    bool Send(const Bytes& data, std::wstring& error) override;
    bool IsConnected() const override;
    void ResizeTerminal(int columns, int rows) override;

private:
    void ReadLoop();
    static std::wstring FindPlink();
    bool CreatePuttySession();
    void DeletePuttySession();
    bool CreateResizeChannel(std::wstring& error);
    void CloseResizeChannel();

    std::wstring host_;
    std::uint16_t port_;
    std::wstring username_;
    std::wstring password_;
    std::wstring hostKey_;
    HostKeyConfirmation confirmHostKey_;
    int terminalColumns_ = 80;
    int terminalRows_ = 24;
    std::wstring puttySessionName_;
    HANDLE process_ = nullptr;
    HANDLE processThread_ = nullptr;
    HANDLE inputWrite_ = nullptr;
    HANDLE outputRead_ = nullptr;
    HANDLE resizeEvent_ = nullptr;
    HANDLE resizeMapping_ = nullptr;
    volatile LONG* resizeState_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::thread readThread_;
    std::mutex writeMutex_;
    DataCallback onData_;
    StatusCallback onStatus_;
};

} // namespace serialctl
