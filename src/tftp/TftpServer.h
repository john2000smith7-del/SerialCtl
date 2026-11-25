#pragma once

#include "TftpFileSource.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace serialctl {

struct TftpTransferStatus {
    uint64_t transferId = 0;
    std::string clientIp;
    uint16_t clientPort = 0;
    std::string tftpName;
    std::string phase;
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
    std::shared_ptr<LocalTftpFileSource> source;
};

enum class TftpRootMode {
    None,
    Local
};

struct TftpRootConfig {
    TftpRootMode mode = TftpRootMode::None;
    std::wstring localDirectory;
};

class TftpServer {
public:
    TftpServer();
    ~TftpServer();

    TftpServer(const TftpServer&) = delete;
    TftpServer& operator=(const TftpServer&) = delete;

    // Server lifecycle (managed tftp32.exe process)
    bool Start(const std::string& bindIp, uint16_t port, std::wstring& error);
    bool Start(const std::string& bindIp, std::wstring& error);
    void Stop();
    bool IsListening() const;
    std::string State() const; // "stopped", "listening"
    std::string BindIp() const;
    uint16_t Port() const;
    std::vector<std::string> LocalIps() const;
    std::string DefaultIp() const;

    // Directory-based publishing (tftp32 BaseDirectory)
    bool SetLocalRoot(const std::wstring& localDirectory, std::wstring& error);
    void ClearRoot();
    TftpRootConfig RootConfig() const;

    // Single-file local publishing (staged under published_root when no local root directory is set)
    bool PublishLocal(const std::string& tftpName, const std::wstring& localPath,
                      bool allowOverwrite, std::wstring& error);
    bool Unpublish(const std::string& tftpName);
    std::vector<TftpPublishedItem> ListPublished() const;
    size_t PublishedCount() const;

    // Legacy transfer query methods kept for MCP compatibility
    std::vector<TftpTransferStatus> ListTransfers() const;
    bool CancelTransfer(uint64_t transferId);
    size_t ActiveTransferCount() const;

    // Static helpers to query local IPv4 addresses via runtime-loaded IPHLPAPI with WS2_32 fallback (no static IPHLPAPI import)
    static std::vector<std::string> EnumerateLocalIps();
    static std::string DefaultLocalIp();

private:
    bool IsProcessAliveLocked() const;
    bool StartLocked(const std::string& bindIp, uint16_t port, std::wstring& error);
    void StopLocked();
    bool EnsureStagingDirLocked(std::wstring& error);
    bool SyncPublishedFilesLocked(std::wstring& error);
    bool WriteIniLocked(const std::wstring& baseDir, const std::string& bindIp, uint16_t port, std::wstring& error);

    mutable std::mutex lifecycleMutex_;
    mutable std::mutex mutex_;
    bool wsaInitialized_ = false;
    std::atomic<bool> running_{false};

    HANDLE jobHandle_ = nullptr;
    HANDLE processHandle_ = nullptr;
    HDESK hiddenDesktop_ = nullptr;
    DWORD processId_ = 0;
    std::wstring stagingDir_;

    std::string bindIp_ = "0.0.0.0";
    uint16_t port_ = 69;
    std::wstring lastError_;

    std::map<std::string, std::shared_ptr<LocalTftpFileSource>> publishedFiles_;
    TftpRootConfig rootConfig_;
};

} // namespace serialctl
