#pragma once

#include <windows.h>
#include <winsock2.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace serialctl {

using Bytes = std::vector<std::uint8_t>;
using DataCallback = std::function<void(const Bytes &)>;
using StatusCallback = std::function<void(const std::wstring &, bool)>;

class IConnection {
  public:
    virtual ~IConnection() = default;
    virtual bool Start(DataCallback onData, StatusCallback onStatus, std::wstring &error) = 0;
    // Only requests cancellation; Stop and resource cleanup stay with the owner.
    virtual void CancelStart() {}
    virtual void Stop() = 0;
    virtual bool Send(const Bytes &data, std::wstring &error) = 0;
    virtual Bytes PrepareInput(const Bytes &data) {
        return data;
    }
    virtual bool SendPrepared(const Bytes &data, std::wstring &error) {
        return Send(data, error);
    }
    virtual bool IsConnected() const = 0;
    virtual unsigned InputCodePage() const {
        return 0;
    }
    virtual unsigned OutputCodePage() const {
        return 0;
    }
    virtual const char *CodePageSource() const {
        return "none";
    }
    // Local terminal encoding selection; zero resumes actual-console detection.
    virtual void OverrideTextCodePage(unsigned) {}
    virtual void ResizeTerminal(int columns, int rows) {
        (void)columns;
        (void)rows;
    }
};

struct SerialSettings {
    std::wstring portName;
    DWORD baudRate = 115200;
    BYTE dataBits = 8;
    BYTE parity = NOPARITY;
    BYTE stopBits = ONESTOPBIT;
    DWORD flowControl = 0;
};

} // namespace serialctl
