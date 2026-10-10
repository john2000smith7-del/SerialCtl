#include "SerialDevice.h"
#include "Win32Helpers.h"

#include <algorithm>
#include <array>

namespace serialctl {

SerialDevice::~SerialDevice() {
    Close();
}

bool SerialDevice::Open(const SerialSettings &settings, DataCallback onData, StatusCallback onStatus,
                        std::wstring &error) {
    if (IsOpen()) {
        error = L"串口已经打开";
        return false;
    }

    std::wstring path = settings.portName;
    if (path.rfind(L"\\\\.\\", 0) != 0) {
        path = L"\\\\.\\" + path;
    }

    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        const auto detail = Win32ErrorMessage(code);
        error = (code == ERROR_FILE_NOT_FOUND  ? L"DEVICE_NOT_FOUND："
                 : code == ERROR_ACCESS_DENIED ? L"DEVICE_BUSY_OR_ACCESS_DENIED（本程序/其他程序占用或权限受限）："
                                               : L"DEVICE_OPEN_FAILED：") +
                settings.portName + L" · " + detail;
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) {
        error = L"读取串口参数失败：" + Win32ErrorMessage();
        CloseHandle(handle);
        return false;
    }

    dcb.BaudRate = settings.baudRate;
    dcb.ByteSize = settings.dataBits;
    dcb.Parity = settings.parity;
    dcb.StopBits = settings.stopBits;
    dcb.fBinary = TRUE;
    dcb.fNull = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fAbortOnError = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fParity = settings.parity != NOPARITY;
    dcb.fOutxCtsFlow = settings.flowControl == 1;
    dcb.fRtsControl = settings.flowControl == 1 ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_DISABLE;
    dcb.fOutX = settings.flowControl == 2;
    dcb.fInX = settings.flowControl == 2;

    if (!SetCommState(handle, &dcb)) {
        error = L"设置串口参数失败：" + Win32ErrorMessage();
        CloseHandle(handle);
        return false;
    }

    COMMTIMEOUTS timeouts{};
    // Win7's documented first-byte mode: available bytes return immediately;
    // an empty queue waits for one byte. The one-second value is an idle read
    // timeout, not a packet batching delay or a measured forwarding latency.
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 1000;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.WriteTotalTimeoutConstant = 1000;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(handle, &timeouts)) {
        error = L"设置串口超时失败：" + Win32ErrorMessage();
        CloseHandle(handle);
        return false;
    }

    SetupComm(handle, 64 * 1024, 64 * 1024);
    // Do not discard bytes received during device setup/startup.

    handle_ = handle;
    onData_ = std::move(onData);
    onStatus_ = std::move(onStatus);
    stopping_ = false;
    connected_ = true;
    readThread_ = std::thread(&SerialDevice::ReadLoop, this);
    return true;
}

void SerialDevice::Close() {
    stopping_ = true;
    connected_ = false;
    if (readThread_.joinable()) {
        CancelSynchronousIo(reinterpret_cast<HANDLE>(readThread_.native_handle()));
        readThread_.join();
    }
    std::lock_guard<std::mutex> writer(writeMutex_);
    HANDLE handle = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    onData_ = {};
    onStatus_ = {};
}

bool SerialDevice::Write(const Bytes &data, std::wstring &error) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (!IsOpen()) {
        error = L"串口尚未打开";
        return false;
    }
    if (data.empty()) {
        return true;
    }
    size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        if (!WriteFile(handle_, data.data() + offset, static_cast<DWORD>(data.size() - offset), &written, nullptr)) {
            error = L"串口写入失败，执行状态未知：" + Win32ErrorMessage();
            return false;
        }
        if (!written) {
            error = L"串口发送超时，执行状态未知；已发送 " + std::to_wstring(offset) + L"/" +
                    std::to_wstring(data.size()) + L" 字节";
            return false;
        }
        // A successful partial write advances this same batch. Other writers
        // cannot interleave because the FIFO worker/write mutex owns it.
        offset += written;
    }
    return true;
}

bool SerialDevice::IsOpen() const {
    return connected_ && handle_ != INVALID_HANDLE_VALUE;
}

void SerialDevice::ReadLoop() {
    std::array<std::uint8_t, 16 * 1024> buffer{};
    while (!stopping_) {
        DWORD read = 0;
        if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            connected_ = false;
            if (!stopping_ && onStatus_) {
                onStatus_(L"读取串口失败：" + Win32ErrorMessage(), true);
            }
            break;
        }
        DWORD total = read;
        while (total < buffer.size()) {
            DWORD errors = 0;
            COMSTAT status{};
            if (!ClearCommError(handle_, &errors, &status)) {
                connected_ = false;
                if (!stopping_ && onStatus_)
                    onStatus_(L"读取串口队列失败：" + Win32ErrorMessage(), true);
                return;
            }
            if (errors && onStatus_) {
                std::wstring message = L"串口驱动错误：";
                if (errors & CE_FRAME)
                    message += L" framing";
                if (errors & CE_RXPARITY)
                    message += L" parity";
                if (errors & CE_OVERRUN)
                    message += L" overrun";
                if (errors & CE_RXOVER)
                    message += L" rx-buffer-overflow";
                if (errors & CE_BREAK)
                    message += L" break";
                onStatus_(message + L" · flags=" + std::to_wstring(errors), true);
            }
            if (status.cbInQue == 0)
                break;
            DWORD extra = 0;
            const DWORD capacity = static_cast<DWORD>(buffer.size()) - total;
            const DWORD requested = std::min(status.cbInQue, capacity);
            if (!ReadFile(handle_, buffer.data() + total, requested, &extra, nullptr)) {
                connected_ = false;
                if (!stopping_ && onStatus_)
                    onStatus_(L"读取串口失败：" + Win32ErrorMessage(), true);
                return;
            }
            if (extra == 0)
                break;
            total += extra;
        }
        if (total > 0 && onData_) {
            onData_(Bytes(buffer.begin(), buffer.begin() + total));
        }
    }
}

} // namespace serialctl
