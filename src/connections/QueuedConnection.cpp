#include "QueuedConnection.h"
#include "Win32Helpers.h"
#include <cstring>
#include <stdexcept>
namespace serialctl {
bool QueuedConnection::Start(DataCallback data, StatusCallback status, std::wstring &error) {
    status_ = status;
    {std::lock_guard<std::mutex> lock(receiveMutex_);normalData_=std::move(data);transferData_={};transferDrain_={};}
    if (!inner_->Start([this](const Bytes& bytes) {
            std::lock_guard<std::mutex> lock(receiveMutex_);
            if(transferData_)transferData_(bytes);else if(normalData_)normalData_(bytes);
        }, status, error))
        return false;
    stopping_ = false;
    resizePending_ = false;
    transferOwner_.clear();writeInFlight_=false;
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
bool QueuedConnection::SendObserved(const Bytes &data, std::wstring &error, std::function<void()> accepted, std::function<bool()> allowed) {
    return Enqueue(data,error,std::move(accepted),std::move(allowed));
}
bool QueuedConnection::SendConditional(const Bytes& data,std::wstring& error,std::function<bool()> allowed) {
    return Enqueue(data,error,{},std::move(allowed));
}
bool QueuedConnection::Enqueue(const Bytes &data,std::wstring& error,std::function<void()> accepted,std::function<bool()> allowed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !IsConnected()) {
        error = L"连接已断开";
        return false;
    }
    if(!transferOwner_.empty()) {error=L"该串口正在传输文件，其他输入暂时禁用";return false;}
    if(allowed&&!allowed())return true;
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
        queue_.push_back({std::move(prepared),std::move(allowed)});
        queued_ += queue_.back().bytes.size();
        wake_.notify_one();
    }
    return true;
}
void QueuedConnection::ResizeTerminal(int columns, int rows) {
    if (!inner_->ResizeTerminalMayBlock()) {
        if (!connected_.load()) return;
        // Preserve SSH's independent mapped channel; do not put its dimensions
        // behind input I/O or preparation under the queue lock. The transport
        // owns channel lifetime when Stop races this atomic state check.
        inner_->ResizeTerminal(columns, rows);
        return;
    }
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
        std::function<bool()> allowed;
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
                bytes = std::move(queue_.front().bytes);
                allowed = std::move(queue_.front().allowed);
                queue_.pop_front();
                queued_ -= bytes.size();
                writeInFlight_=true;
            }
        }
        if (resize) inner_->ResizeTerminal(columns, rows);
        if (!hasData) continue;
        if(allowed&&!allowed()) {std::lock_guard<std::mutex> lock(mutex_);writeInFlight_=false;continue;}
        std::wstring error;
        const bool sent=inner_->SendPrepared(bytes,error);
        {std::lock_guard<std::mutex> lock(mutex_);writeInFlight_=false;}
        if (!sent) {
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
void QueuedConnection::ReleaseCapture() {
    std::lock_guard<std::mutex> lock(receiveMutex_);
    Bytes remainder=transferDrain_?transferDrain_():Bytes{};
    transferData_={};transferDrain_={};
    if(!remainder.empty()&&normalData_)normalData_(remainder);
}
void QueuedConnection::CancelTransfer(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!owner.empty()&&transferOwner_==owner)inner_->CancelTransferWire(owner);
}
void QueuedConnection::AbortTransfer(const std::string& owner) {
    std::lock_guard<std::mutex> writer(transferWriteMutex_);
    {std::lock_guard<std::mutex> lock(mutex_);if(stopping_||owner.empty()||transferOwner_!=owner)return;}
    inner_->AbortTransferWire(owner);
}
bool QueuedConnection::BeginTransfer(const std::string& owner,DataCallback capture,std::wstring& error,std::function<Bytes()> drain) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(owner.empty()||stopping_||!IsConnected()){error=L"串口已断开或传输标识无效";return false;}
        if(!transferOwner_.empty()||writeInFlight_||!queue_.empty()){error=L"该串口仍有发送任务，请等待完成后再传输文件";return false;}
        transferOwner_=owner;
    }
    {std::lock_guard<std::mutex> lock(receiveMutex_);transferData_=std::move(capture);transferDrain_=std::move(drain);}
    if(inner_->BeginTransferWire(owner,error))return true;
    ReleaseCapture();
    {std::lock_guard<std::mutex> lock(mutex_);transferOwner_.clear();}
    return false;
}
bool QueuedConnection::SendTransfer(const std::string& owner,const Bytes& bytes,std::wstring& error) {
    std::lock_guard<std::mutex> writer(transferWriteMutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_||!IsConnected()||transferOwner_!=owner||owner.empty()) {error=L"传输已停止或串口所有权已变化";return false;}
    }
    return inner_->SendTransferWire(owner,bytes,error);
}
bool QueuedConnection::EndTransfer(const std::string& owner,std::wstring& error) {
    std::lock_guard<std::mutex> writer(transferWriteMutex_);
    {std::lock_guard<std::mutex> lock(mutex_);if(owner.empty()||transferOwner_!=owner){error=L"传输所有权已变化";return false;}}
    const bool released=inner_->EndTransferWire(owner,error);
    ReleaseCapture();
    {std::lock_guard<std::mutex> lock(mutex_);transferOwner_.clear();}
    return released;
}

} // namespace serialctl
