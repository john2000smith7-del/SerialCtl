#pragma once
#include "ByteTypes.h"

#include <windows.h>
#include <winsock2.h>

#include <cstdint>
#include <functional>
#include <string>

namespace serialctl {

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
    virtual bool SendConditional(const Bytes& data,std::wstring& error,std::function<bool()> allowed) {
        return !allowed||allowed()?Send(data,error):true;
    }
    virtual bool BeginTransferWire(const std::string&,std::wstring& error) {
        error=L"当前连接不支持独占文件传输";return false;
    }
    virtual bool SupportsTransfer() const {return false;}
    virtual bool SendTransferWire(const std::string&,const Bytes& bytes,std::wstring& error) {return SendPrepared(bytes,error);}
    virtual bool EndTransferWire(const std::string&,std::wstring&) {return true;}
    virtual void CancelTransferWire(const std::string&) {}
    virtual void AbortTransferWire(const std::string& owner) {
        std::wstring ignored;SendTransferWire(owner,Bytes{24,24,24,24,24,24,24,24,8,8,8,8,8,8,8,8},ignored);
    }
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
    // Independent channels (for example SSH's mapped size/event) stay
    // immediate even when input is blocked. Wire-based resize opts into the
    // sender queue so native layout never waits for socket I/O.
    virtual bool ResizeTerminalMayBlock() const { return false; }
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
    std::wstring friendlyName;
};

} // namespace serialctl
