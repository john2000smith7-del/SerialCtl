#pragma once

#include "TftpFileSource.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

namespace serialctl {

struct TftpTransferStatus {
    uint64_t transferId = 0;
    std::string clientIp;
    uint16_t clientPort = 0;
    std::string tftpName;
    std::string phase; // "connecting", "transferring", "completed", "failed", "cancelled"
    uint64_t sentBytes = 0;
    uint64_t ackedBytes = 0;
    uint64_t totalBytes = 0;
    uint32_t retransmits = 0;
    double speedBps = 0.0;
    std::wstring error;
    uint64_t startTimeMs = 0;
    uint64_t finishTimeMs = 0;
};

struct TftpPublishedItem {
    std::string tftpName;
    std::shared_ptr<ITftpFileSource> source;
};

class TftpTransferSession;

class TftpServer {
public:
    static constexpr size_t kMaxConcurrentTftpSessions = 16;

    TftpServer();
    ~TftpServer();

    // Server lifecycle
    bool Start(const std::string& bindIp, uint16_t port, std::wstring& error);
    void Stop();
    bool IsListening() const;
    std::string State() const; // "stopped", "listening", "transferring", "failed"
    std::string BindIp() const;
    uint16_t Port() const;
    std::vector<std::string> LocalIps() const;

    // File publishing
    bool PublishLocal(const std::string& tftpName, const std::wstring& localPath,
                      bool allowOverwrite, std::wstring& error);
    bool PublishRemote(const std::string& tftpName, const SshRemoteConfig& config,
                       bool allowOverwrite, std::wstring& error);
    bool PublishCustom(const std::string& tftpName, std::shared_ptr<ITftpFileSource> source,
                       bool allowOverwrite, std::wstring& error);
    bool Unpublish(const std::string& tftpName);
    std::vector<TftpPublishedItem> ListPublished() const;
    size_t PublishedCount() const;

    // Transfers & diagnostics
    std::vector<TftpTransferStatus> ListTransfers() const;
    bool CancelTransfer(uint64_t transferId);
    size_t ActiveTransferCount() const;

    // Static helper to query local IPv4 addresses via WS2_32 (No IPHLPAPI dependency)
    static std::vector<std::string> EnumerateLocalIps();

private:
    void ListenerThread();
    void CleanCompletedSessions();

    mutable std::mutex mutex_;
    SOCKET listener_ = INVALID_SOCKET;
    std::thread listenerThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    std::string bindIp_ = "0.0.0.0";
    uint16_t port_ = 69;
    std::wstring lastError_;

    std::map<std::string, std::shared_ptr<ITftpFileSource>> publishedFiles_;
    std::vector<std::shared_ptr<TftpTransferSession>> sessions_;
    uint64_t nextTransferId_ = 1;
};

} // namespace serialctl
