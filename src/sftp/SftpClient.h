#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace serialctl {

struct SftpEntry {
    std::wstring name;
    bool directory = false;
    std::uint64_t size = 0;
    std::wstring permissions;
    std::wstring modifiedText;
    std::uint64_t modifiedSortKey = 0;
    bool symlink = false;
    std::wstring linkTarget;
    bool pathSafe = true;
};

struct SftpListing {
    std::wstring directory;
    std::vector<SftpEntry> entries;
};

struct SftpDeleteTarget {
    std::wstring path;
    bool directory = false;
};

struct SftpTransferProgress {
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    std::uint64_t bytesPerSecond = 0;
    std::uint32_t remainingSeconds = 0;
    int percent = -1;
};

using SftpProgressCallback = std::function<void(const SftpTransferProgress&)>;

class SftpClient {
public:
    SftpClient(std::wstring host, std::uint16_t port, std::wstring username, std::wstring password);

    static bool IsAvailable();
    bool List(const std::wstring& directory, SftpListing& listing, std::wstring& error,
        const std::atomic_bool* cancel = nullptr) const;
    bool Upload(const std::wstring& localPath, const std::wstring& remoteDirectory,
        bool replaceExisting, std::wstring& error, const SftpProgressCallback& progress = {},
        const std::atomic_bool* cancel = nullptr) const;
    bool Download(const std::wstring& remotePath, const std::wstring& localPath,
        std::uint64_t expectedSize, bool replaceExisting, std::wstring& error,
        const SftpProgressCallback& progress = {}, const std::atomic_bool* cancel = nullptr) const;
    bool CreateDirectory(const std::wstring& path, std::wstring& error,
        const std::atomic_bool* cancel = nullptr) const;
    bool Rename(const std::wstring& source, const std::wstring& destination,
        std::wstring& error, const std::atomic_bool* cancel = nullptr) const;
    bool Delete(const std::vector<SftpDeleteTarget>& targets, std::wstring& error,
        const std::atomic_bool* cancel = nullptr) const;
    bool ChangeMode(const std::vector<std::wstring>& paths, const std::wstring& mode,
        std::wstring& error, const std::atomic_bool* cancel = nullptr) const;

private:
    enum class TransferKind { None, Upload, Download };
    bool RunBatch(const std::wstring& commands, DWORD timeoutMilliseconds,
        std::wstring& output, std::wstring& error, TransferKind transferKind = TransferKind::None,
        std::uint64_t totalBytes = 0, const SftpProgressCallback& progress = {},
        const std::atomic_bool* cancel = nullptr) const;
    static std::wstring FindPsftp();
    static std::wstring FindPlink();

    std::wstring host_;
    std::uint16_t port_;
    std::wstring username_;
    std::wstring password_;
};

} // namespace serialctl
