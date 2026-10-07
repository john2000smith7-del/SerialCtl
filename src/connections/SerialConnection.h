#pragma once

#include "Connection.h"
#include "SerialDevice.h"

namespace serialctl {

class SerialConnection final : public IConnection {
public:
    explicit SerialConnection(SerialSettings settings);
    bool Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) override;
    void Stop() override;
    bool Send(const Bytes& data, std::wstring& error) override;
    bool IsConnected() const override;

private:
    SerialSettings settings_;
    SerialDevice device_;
    StatusCallback onStatus_;
};

} // namespace serialctl
