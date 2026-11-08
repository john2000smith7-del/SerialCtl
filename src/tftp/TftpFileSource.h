#pragma once

#include "PasswordPipe.h"
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <thread>
#include <windows.h>

namespace serialctl {

struct TftpSourceInfo {
    std::string type;            // "local" or "remote"
    std::wstring displayPath;    // Sanitized display path (passwords stripped)
    std::wstring host;           // Remote host (for remote)
    uint16_t port = 0;           // Remote port
    std::wstring username;       // Remote username
    uint64_t size = 0;
    bool sizeKnown = false;
    uint64_t lastModified = 0;
    std::string status = "Ready"; // "Ready", "Failed", "Unavailable"
    std::wstring error;
};

class ITftpFileSource {
public:
    virtual ~ITftpFileSource() = default;
    virtual TftpSourceInfo Info() const = 0;
    virtual std::shared_ptr<ITftpFileSource> CreateSessionSource() const = 0;
    virtual bool Open(std::wstring& error) = 0;
    virtual void Close() = 0;
    // Reads up to length bytes at given offset.
    // bytesRead == 0 indicates EOF. Returns false on error.
    virtual bool Read(uint64_t offset, void* buffer, size_t length, size_t& bytesRead, std::wstring& error) = 0;
    virtual void Cancel() = 0;
};

// 1. Local file source: direct offset reads with modification checks. Zero temporary files.
class LocalTftpFileSource : public ITftpFileSource {
public:
    explicit LocalTftpFileSource(std::wstring path);
    ~LocalTftpFileSource() override;

    TftpSourceInfo Info() const override;
    std::shared_ptr<ITftpFileSource> CreateSessionSource() const override;
    bool Open(std::wstring& error) override;
    void Close() override;
    bool Read(uint64_t offset, void* buffer, size_t length, size_t& bytesRead, std::wstring& error) override;
    void Cancel() override;

private:
    LocalTftpFileSource(std::wstring path, BY_HANDLE_FILE_INFORMATION snapshot, TftpSourceInfo info);
    std::wstring path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION snapshot_{};
    TftpSourceInfo info_;
    mutable std::mutex mutex_;
};

struct SshRemoteConfig {
    std::wstring host;
    uint16_t port = 22;
    std::wstring username;
    std::wstring password;
    std::wstring remotePath;
    std::wstring hostKey;
};

// 2. Remote SSH/SFTP streaming source: true zero-disk footprint with bounded sliding window.
class RemoteSshTftpFileSource : public ITftpFileSource {
public:
    // Pluggable launcher for tests / production
    using StreamLauncher = std::function<bool(
        const SshRemoteConfig& config,
        HANDLE& outputRead,
        HANDLE& procHandle,
        std::wstring& error)>;

    RemoteSshTftpFileSource(SshRemoteConfig config, StreamLauncher customLauncher = nullptr);
    ~RemoteSshTftpFileSource() override;

    TftpSourceInfo Info() const override;
    std::shared_ptr<ITftpFileSource> CreateSessionSource() const override;
    bool Open(std::wstring& error) override;
    void Close() override;
    bool Read(uint64_t offset, void* buffer, size_t length, size_t& bytesRead, std::wstring& error) override;
    void Cancel() override;

    // For testing and metrics verification
    size_t CurrentBufferSize() const;
    static size_t MaxBufferCapacity() { return 2 * 1024 * 1024; } // 2 MiB bounded buffer

private:
    RemoteSshTftpFileSource(SshRemoteConfig config, StreamLauncher customLauncher, TftpSourceInfo prequeriedInfo,
                           std::wstring snapshot);
    void ReaderThread();
    bool QueryRemoteAttributes(std::wstring& error);

    SshRemoteConfig config_;
    StreamLauncher launcher_;
    TftpSourceInfo info_;
    std::wstring snapshot_;

    PasswordPipe passwordPipe_;
    HANDLE procHandle_ = nullptr;
    HANDLE outputRead_ = nullptr;
    std::thread readerThread_;
    std::mutex lifecycleMutex_;

    mutable std::mutex bufferMutex_;
    std::condition_variable cvBuffer_;
    std::deque<uint8_t> buffer_;
    uint64_t bufferStartOffset_ = 0;
    uint64_t streamReadOffset_ = 0;
    std::atomic<bool> cancelled_{false};
    bool streamEnded_ = false;
    std::wstring streamError_;
};

// 3. Virtual simulated stream source (specifically for unit & integration testing 500 MiB+ without live SSH)
class MockStreamTftpFileSource : public ITftpFileSource, public std::enable_shared_from_this<MockStreamTftpFileSource> {
public:
    using StreamGenerator = std::function<void(uint64_t offset, uint8_t* dest, size_t len)>;

    MockStreamTftpFileSource(uint64_t totalSize, StreamGenerator generator = nullptr);
    ~MockStreamTftpFileSource() override;

    TftpSourceInfo Info() const override;
    std::shared_ptr<ITftpFileSource> CreateSessionSource() const override;
    bool Open(std::wstring& error) override;
    void Close() override;
    bool Read(uint64_t offset, void* buffer, size_t length, size_t& bytesRead, std::wstring& error) override;
    void Cancel() override;

    void InjectError(const std::wstring& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        injectedError_ = error;
    }

private:
    uint64_t totalSize_ = 0;
    StreamGenerator generator_;
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::wstring injectedError_;
};

} // namespace serialctl
