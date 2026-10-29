#pragma once

#include "Connection.h"
#include "CancellableIo.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace serialctl {

class SerialDevice {
public:
    SerialDevice() = default;
    ~SerialDevice();

    bool Open(const SerialSettings& settings, DataCallback onData, StatusCallback onStatus, std::wstring& error);
    void Close();
    bool Write(const Bytes& data, std::wstring& error);
    bool IsOpen() const;

private:
    void ReadLoop();

    std::atomic_bool connected_{false};
    std::atomic<HANDLE> handle_{INVALID_HANDLE_VALUE};
    std::atomic<bool> stopping_{false};
    std::thread readThread_;
    std::mutex writeMutex_;
    HANDLE stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CancellableIo readIo_, writeIo_;
    DataCallback onData_;
    StatusCallback onStatus_;
};

} // namespace serialctl
