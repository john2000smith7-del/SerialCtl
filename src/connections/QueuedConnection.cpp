#include "QueuedConnection.h"
#include "Win32Helpers.h"
#include <cstring>
#include <stdexcept>
namespace serialctl {
bool QueuedConnection::Start(DataCallback data, StatusCallback status, std::wstring &error) {
    status_ = status;
    if (!inner_->Start(std::move(data), status, error))
        return false;
    stopping_ = false;
    resizePending_ = false;
    connected_ = true;
    worker_ = std::thread(&QueuedConnection::Run, this);
    return true;
}
void QueuedConnection::Stop() {
    connected_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
        queued_ = 0;
        resizePending_ = false;
        wake_.notify_all();
    }
    if (worker_.joinable())
        CancelSynchronousIo(reinterpret_cast<HANDLE>(worker_.native_handle()));
    inner_->Stop(); // Cancel transport I/O before joining a possibly blocked writer.
    if (worker_.joinable()) {
        CancelSynchronousIo(reinterpret_cast<HANDLE>(worker_.native_handle()));
        // TCP transports have a finite send timeout; pipe writes are canceled above.
        worker_.join();
    }
    status_ = {};
}
bool QueuedConnection::Send(const Bytes &data, std::wstring &error) {
    return SendObserved(data, error, {});
}
bool QueuedConnection::SendObserved(const Bytes &data, std::wstring &error, std::function<void()> accepted) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !IsConnected()) {
        error = L"连接已断开";
        return false;
    }
    if (data.size() > 8 * 1024 * 1024) {
        error = L"发送队列已满，请稍后重试";
        return false;
    }
    Bytes prepared;
    try {
        prepared = inner_->PrepareInput(data);
    } catch (const std::exception &ex) {
        error = std::wstring(L"输入编码或批次无效：") +
                MultiByteToWide(reinterpret_cast<const std::uint8_t *>(ex.what()), strlen(ex.what()), CP_UTF8);
        return false;
    }
    if (prepared.size() > 8 * 1024 * 1024 || queued_ + prepared.size() > 8 * 1024 * 1024) {
        error = L"发送队列已满，请稍后重试";
        return false;
    }
    if (!data.empty()) {
        if (accepted)
            accepted();
        queue_.push_back(std::move(prepared));
        queued_ += queue_.back().size();
        wake_.notify_one();
    }
    return true;
}
void QueuedConnection::ResizeTerminal(int columns, int rows) {
    // Telnet NAWS can wait on the wire mutex/socket just like terminal input.
    // Native layout must only publish the latest dimensions, never send them.
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    resizeColumns_ = columns;
    resizeRows_ = rows;
    resizePending_ = true;
    wake_.notify_one();
}
void QueuedConnection::Run() {
    for (;;) {
        Bytes bytes;
        bool resize = false, hasData = false;
        int columns = 0, rows = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || resizePending_ || !queue_.empty(); });
            if (stopping_)
                break;
            resize = resizePending_;
            columns = resizeColumns_;
            rows = resizeRows_;
            resizePending_ = false;
            // Handle at most one latest size plus one FIFO input each turn;
            // repeated layout cannot create an unbounded queue or starve input.
            hasData = !queue_.empty();
            if (hasData) {
                bytes = std::move(queue_.front());
                queue_.pop_front();
                queued_ -= bytes.size();
            }
        }
        if (resize) inner_->ResizeTerminal(columns, rows);
        if (!hasData) continue;
        std::wstring error;
        if (!inner_->SendPrepared(bytes, error)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.clear();
                queued_ = 0;
                stopping_ = true;
            }
            connected_ = false;
            if (status_)
                status_(error, true);
            break;
        }
    }
}
} // namespace serialctl
