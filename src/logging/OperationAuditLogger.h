#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace serialctl {

// Circular JSONL audit log for manual, AI, and MCP actions stored under
// <exe_dir>\logs\operations\ (with AppData fallback via SessionLogger).
class OperationAuditLogger {
public:
    static OperationAuditLogger& Instance();

    OperationAuditLogger();
    ~OperationAuditLogger();

    OperationAuditLogger(const OperationAuditLogger&) = delete;
    OperationAuditLogger& operator=(const OperationAuditLogger&) = delete;

    void Configure(const std::wstring& directory, size_t maxFiles = 10,
                   std::uint64_t maxFileBytes = 4ull * 1024 * 1024);
    std::wstring Directory() const;

    void Record(const std::string& category, const std::string& action,
                const std::string& target = {}, const std::string& summary = {},
                const std::string& result = "ok");

    void Flush();
    static void EmergencyFlushAll();

    bool ExportTo(const std::wstring& destinationFile, std::wstring& error) const;
    bool ClearHistory(std::wstring& error);

    void SetAutoDeleteOnExit(bool enabled);
    bool AutoDeleteOnExit() const;
    void Shutdown();

    std::uint64_t RecordedCount() const;

private:
    bool EnsureOpenLocked(std::wstring* error = nullptr);
    void CloseLocked();
    std::vector<std::wstring> EnumeratePartsLocked() const;
    void PruneOldFilesLocked();
    static std::string CurrentTimestampUtf8();

    mutable std::mutex mutex_;
    std::wstring directory_;
    size_t maxFiles_ = 10;
    std::uint64_t maxFileBytes_ = 4ull * 1024 * 1024;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::atomic<HANDLE> activeHandle_{INVALID_HANDLE_VALUE};
    std::wstring activePath_;
    std::uint64_t activeBytes_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t recordedCount_ = 0;
    bool autoDeleteOnExit_ = false;
};

} // namespace serialctl
