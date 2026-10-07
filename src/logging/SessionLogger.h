#pragma once

#include "Connection.h"

#include <mutex>
#include <string>

namespace serialctl {

class SessionLogger {
public:
    ~SessionLogger();
    bool Start(const std::wstring& mode, std::wstring& error);
    void WriteRaw(const Bytes& data);
    void WriteText(const std::wstring& text);
    void WriteStatus(const std::wstring& text);
    bool SaveCopy(const std::wstring& destination, std::wstring& error);
    void Stop();
    const std::wstring& Path() const { return path_; }

private:
    void WriteBytes(const void* data, DWORD size);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::mutex mutex_;
    std::wstring path_;
};

} // namespace serialctl
