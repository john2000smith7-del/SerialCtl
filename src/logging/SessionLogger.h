#pragma once

#include "Connection.h"

#include <condition_variable>
#include <atomic>
#include <functional>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace serialctl
{

class SessionLogger
{
  public:
    ~SessionLogger();
    bool Start(const std::wstring &mode, std::wstring &error, bool csv = false);
    // After a connection is stopped, reopen the same log/parts rather than discard
    // history and the user's continuous-save destination.
    bool Resume(const std::wstring &mode, std::wstring &error);
    bool StartContinuousSave(const std::wstring &destination, std::wstring &error);
    void StopContinuousSave();
    bool ContinuousSaving() const { return continuousActive_.load(); }
    std::wstring ContinuousSaveError() const;
    void WriteRaw(const Bytes &data);
    void WriteText(const std::wstring &text);
    void WriteStatus(const std::wstring &text);
    void WriteBatch(const std::vector<std::pair<bool, std::wstring>>& entries);
    struct Statistics {
        size_t pendingBytes = 0;
        size_t highWaterBytes = 0;
        std::uint64_t acceptedBytes = 0;
        std::uint64_t completedBytes = 0;
        std::uint64_t overflowEvents = 0;
    };
    Statistics Stats() const;
    struct ExportPoint {
        std::uint64_t generation = 0, bytes = 0;
        std::wstring root;
    };
    using ExportProgress = std::function<void(std::uint64_t, std::uint64_t)>;
    ExportPoint CaptureExportPoint();
    bool SaveCopy(const std::wstring &destination, std::wstring &error);
    bool SaveCopy(const std::wstring &destination, ExportPoint point, std::wstring &error,
                  const std::atomic<bool> *cancel, const ExportProgress &progress = {});
    void Stop();
    std::wstring Error();
    // Returned by value under the lock: Start()/Stop() rewrite path_ on other threads.
    std::wstring Path() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return path_;
    }

  private:
    struct Part {
        std::wstring path;
        std::uint64_t length = 0;
    };
    struct Snapshot {
        std::wstring root;
        std::vector<Part> parts;
        std::uint64_t length = 0;
    };
    bool CaptureSnapshot(ExportPoint, const std::atomic<bool> *, Snapshot &, std::wstring &);
    static bool CopySnapshot(const Snapshot &, const std::wstring &, std::wstring &,
                             const std::atomic<bool> *, const ExportProgress &);
    bool CopyCompletedRange(ExportPoint, std::uint64_t from, HANDLE output, std::wstring& error);
    void ContinuousSaveWorker(HANDLE output);
    void WriteBytes(const void *data, DWORD size);
    void Run();
    std::deque<Bytes> queue_;
    size_t queued_ = 0;
    size_t highWaterQueued_ = 0;
    std::uint64_t overflowEvents_ = 0;
    bool stopping_ = true, writing_ = false, writerAlive_ = false;
    std::wstring failure_;
    std::thread worker_;
    std::condition_variable wake_;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    mutable std::mutex mutex_;
    std::mutex encodeMutex_;
    std::wstring path_;
    std::vector<Part> parts_;
    mutable std::mutex continuousMutex_;
    std::condition_variable continuousWake_;
    std::thread continuousWorker_;
    std::atomic<bool> continuousStop_{false}, continuousActive_{false};
    std::atomic<std::uint64_t> continuousStopAt_{0};
    std::wstring continuousPath_, continuousError_;
    std::uint64_t written_ = 0;
    std::uint64_t generation_ = 0, accepted_ = 0, completed_ = 0;
#ifdef SERIALCTL_LOGGER_TESTING
    friend struct SessionLoggerTestAccess;
    static std::atomic<std::int64_t> testCopyFailureAfter_;
    static std::atomic<bool> testFailFlushCopy_;
    static std::atomic<bool> testFailFlushStop_;
#endif
};

} // namespace serialctl
