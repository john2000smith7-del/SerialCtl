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
    bool SendObserved(const Bytes &, std::wstring &, std::function<void()> accepted);
    void ResizeTerminal(int c, int r) override {
        inner_->ResizeTerminal(c, r);
    }

  private:
    void Run();
    std::unique_ptr<IConnection> inner_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Bytes> queue_;
    size_t queued_ = 0;
    bool stopping_ = true;
    std::atomic_bool connected_{false};
    std::thread worker_;
    StatusCallback status_;
};
} // namespace serialctl
