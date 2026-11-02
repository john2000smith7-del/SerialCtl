#pragma once

#include "Connection.h"
#include "SerialDevice.h"

namespace serialctl {

class SerialConnection final : public IConnection {
public:
    explicit SerialConnection(SerialSettings settings);
    ~SerialConnection() override;
    bool Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) override;
    void Stop() override;
    bool Send(const Bytes& data, std::wstring& error) override;
    bool IsConnected() const override;
    bool SupportsTransfer() const override {return true;}
    bool BeginTransferWire(const std::string&,std::wstring& error) override {
        if(IsConnected()&&transferCancel_){ResetEvent(transferCancel_);return true;}error=L"串口已断开或取消事件不可用";return false;
    }
    bool SendTransferWire(const std::string&,const Bytes& bytes,std::wstring& error) override {return device_.Write(bytes,error,transferCancel_);}
    void CancelTransferWire(const std::string&) override {if(transferCancel_)SetEvent(transferCancel_);}
    void AbortTransferWire(const std::string&) override {
        if(transferCancel_)ResetEvent(transferCancel_);
        std::wstring ignored;device_.Write(Bytes{24,24,24,24,24,24,24,24,8,8,8,8,8,8,8,8},ignored,transferCancel_);
    }

private:
    SerialSettings settings_;
    SerialDevice device_;
    StatusCallback onStatus_;
    HANDLE transferCancel_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
};

} // namespace serialctl
