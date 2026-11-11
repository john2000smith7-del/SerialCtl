#pragma once
#include "Connection.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
namespace serialctl {
// Owns one transport. All terminal writers share one bounded FIFO.
class QueuedConnection final : public IConnection {
  public:
    explicit QueuedConnection(std::unique_ptr<IConnection> inner) : inner_(std::move(inner)) {}
    ~QueuedConnection() override {
        Stop();
    }
    IConnection *Transport() {
        return inner_.get();
    }
    bool Start(DataCallback data, StatusCallback status, std::wstring &error) override;
    void CancelStart() override {
        inner_->CancelStart();
    }
    void Stop() override;
    bool Send(const Bytes &data, std::wstring &error) override;
    bool Interrupt(std::wstring &error) override;
    bool SendConditional(const Bytes&,std::wstring&,std::function<bool()>) override;
    bool SupportsTransfer() const override {return inner_->SupportsTransfer();}
    bool TransferActive() const {std::lock_guard<std::mutex> lock(mutex_);return !transferOwner_.empty();}
    bool BeginTransfer(const std::string&,DataCallback,std::wstring&,std::function<Bytes()> drain={});
    bool SendTransfer(const std::string&,const Bytes&,std::wstring&);
    bool EndTransfer(const std::string&,std::wstring&);
    void CancelTransfer(const std::string&);
    void AbortTransfer(const std::string&);
    bool IsConnected() const override {
        return connected_ && inner_->IsConnected();
    }
    unsigned InputCodePage() const override {
        return inner_->InputCodePage();
    }
    unsigned OutputCodePage() const override {
        return inner_->OutputCodePage();
    }
    const char *CodePageSource() const override {
        return inner_->CodePageSource();
    }
    void OverrideTextCodePage(unsigned page) override {
        inner_->OverrideTextCodePage(page);
    }
    enum class WriteResult { Written, NotWritten, Unknown };
    using WriteCompletion = std::function<void(WriteResult, const std::wstring &error)>;
    bool SendObserved(const Bytes &, std::wstring &, std::function<void()> accepted,
                      std::function<bool()> allowed = {}, WriteCompletion completion = {});
    void ResizeTerminal(int columns, int rows) override;

  private:
    struct Write {
        Bytes bytes;
        std::function<bool()> allowed;
        WriteCompletion completion;
    };
    bool Enqueue(const Bytes &, std::wstring &, std::function<void()>, std::function<bool()>, WriteCompletion);
    void ReleaseCapture();
    void Run();
    std::unique_ptr<IConnection> inner_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Write> queue_;
    std::mutex receiveMutex_,transferWriteMutex_;
    DataCallback normalData_,transferData_;
    std::function<Bytes()> transferDrain_;
    std::string transferOwner_;
    bool writeInFlight_=false;
    size_t queued_ = 0;
    bool stopping_ = true, resizePending_ = false;
    int resizeColumns_ = 80, resizeRows_ = 24;
    std::atomic_bool connected_{false};
    std::thread worker_;
    StatusCallback status_;
};
} // namespace serialctl
