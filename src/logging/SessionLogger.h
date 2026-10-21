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
    void WriteRaw(const Bytes &data);
    void WriteText(const std::wstring &text);
    void WriteStatus(const std::wstring &text);
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
    const std::wstring &Path() const
    {
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
    void WriteBytes(const void *data, DWORD size);
    void Run();
    std::deque<Bytes> queue_;
    size_t queued_ = 0;
    bool stopping_ = true, writing_ = false, writerAlive_ = false;
    std::wstring failure_;
    std::thread worker_;
    std::condition_variable wake_;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::mutex mutex_;
    std::wstring path_;
    std::vector<Part> parts_;
    std::uint64_t written_ = 0;
    std::uint64_t generation_ = 0, accepted_ = 0, completed_ = 0;
#ifdef SERIALCTL_LOGGER_TESTING
    friend struct SessionLoggerTestAccess;
    static std::atomic<std::int64_t> testCopyFailureAfter_;
#endif
};

} // namespace serialctl
