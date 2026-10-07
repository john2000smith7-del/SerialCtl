#include "SerialConnection.h"

namespace serialctl {

SerialConnection::SerialConnection(SerialSettings settings) : settings_(std::move(settings)) {}

bool SerialConnection::Start(DataCallback onData, StatusCallback onStatus, std::wstring& error) {
    onStatus_ = onStatus;
    if (!device_.Open(settings_, std::move(onData), onStatus, error)) {
        return false;
    }
    if (onStatus_) {
        onStatus_(L"串口已连接：" + settings_.portName, false);
    }
    return true;
}

void SerialConnection::Stop() {
    const bool wasOpen = device_.IsOpen();
    device_.Close();
    if (wasOpen && onStatus_) {
        onStatus_(L"串口已断开", false);
    }
}

bool SerialConnection::Send(const Bytes& data, std::wstring& error) {
    return device_.Write(data, error);
}

bool SerialConnection::IsConnected() const {
    return device_.IsOpen();
}

} // namespace serialctl
