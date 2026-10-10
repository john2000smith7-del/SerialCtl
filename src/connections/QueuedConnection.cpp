#include "QueuedConnection.h"
namespace serialctl
{
bool QueuedConnection::Start(DataCallback data, StatusCallback status, std::wstring &error)
{
    status_ = status;
    if (!inner_->Start(std::move(data), status, error))
        return false;
    stopping_ = false;
    connected_ = true;
    worker_ = std::thread(&QueuedConnection::Run, this);
    return true;
}
void QueuedConnection::Stop()
{
    connected_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
        queued_ = 0;
        wake_.notify_all();
    }
    if (worker_.joinable())
    {
        CancelSynchronousIo(reinterpret_cast<HANDLE>(worker_.native_handle()));
        // TCP transports have a finite send timeout; pipe writes are canceled above.
        worker_.join();
    }
    inner_->Stop();
    status_ = {};
}
bool QueuedConnection::Send(const Bytes &data, std::wstring &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !IsConnected())
    {
        error = L"连接已断开";
        return false;
    }
    if (data.size() > 8 * 1024 * 1024 || queued_ + data.size() > 8 * 1024 * 1024)
    {
        error = L"发送队列已满，请稍后重试";
        return false;
    }
    if (!data.empty())
    {
        queue_.push_back(data);
        queued_ += data.size();
        wake_.notify_one();
    }
    return true;
}
void QueuedConnection::Run()
{
    for (;;)
    {
        Bytes bytes;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
                break;
            bytes = std::move(queue_.front());
            queue_.pop_front();
            queued_ -= bytes.size();
        }
        std::wstring error;
        if (!inner_->Send(bytes, error))
        {
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
