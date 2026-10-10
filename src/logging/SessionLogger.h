#pragma once

#include "Connection.h"

#include <condition_variable>
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
    bool SaveCopy(const std::wstring &destination, std::wstring &error);
    void Stop();
    std::wstring Error();
    const std::wstring &Path() const
    {
        return path_;
    }

  private:
    void WriteBytes(const void *data, DWORD size);
    void Run();
    std::deque<Bytes> queue_;
    size_t queued_ = 0;
    bool stopping_ = true, writing_ = false;
    std::wstring failure_;
    std::thread worker_;
    std::condition_variable wake_;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::mutex mutex_;
    std::wstring path_;
    std::vector<std::wstring> parts_;
    std::uint64_t written_ = 0;
};

} // namespace serialctl
