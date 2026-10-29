#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace serialctl {
// Own cleanup work until completion; never detach transports from their owner.
// Callers unregister callbacks/resources before retiring them and Drain before
// destroying callback targets. Tasks must not capture UI objects.
class BackgroundCleanup {
public:
    ~BackgroundCleanup() { Drain(); }
    bool Busy() const { std::lock_guard<std::mutex> lock(mutex_); return pending_ != 0; }
    void Retire(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
        tasks_.push_back(std::move(task)); ++pending_; wake_.notify_one();
    }
    void Drain() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; wake_.notify_all(); }
        if (worker_.joinable()) worker_.join();
    }
private:
    void Run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (tasks_.empty()) return;
                task = std::move(tasks_.front()); tasks_.pop_front();
            }
            task();
            { std::lock_guard<std::mutex> lock(mutex_); --pending_; }
        }
    }
    mutable std::mutex mutex_;
    size_t pending_ = 0;
    std::condition_variable wake_;
    std::deque<std::function<void()>> tasks_;
    std::thread worker_;
    bool stopping_ = false;
};
} // namespace serialctl
