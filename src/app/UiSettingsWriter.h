#pragma once

#include <windows.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace serialctl {

// Settings are complete snapshots, so an unwritten older snapshot may be
// replaced by a newer one. The worker never holds references to UI objects.
class UiSettingsWriter {
public:
    struct Entry { std::wstring section, key, value; };
    struct Snapshot { std::wstring path; std::vector<Entry> entries; };
    using Write = std::function<void(const Snapshot &)>;

    explicit UiSettingsWriter(Write write = WriteProfile)
        : write_(std::move(write)), worker_([this] { Run(); }) {}
    ~UiSettingsWriter() { Stop(); }
    UiSettingsWriter(const UiSettingsWriter &) = delete;
    UiSettingsWriter &operator=(const UiSettingsWriter &) = delete;

    bool Submit(Snapshot snapshot) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return false;
            pending_ = std::move(snapshot);
        }
        ready_.notify_one();
        return true;
    }

    // Flushes the last accepted snapshot. Call from the owned shutdown worker,
    // not the live window message handler, because storage may be slow.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

private:
    static void WriteProfile(const Snapshot &snapshot) {
        for (const auto &entry : snapshot.entries)
            WritePrivateProfileStringW(entry.section.c_str(), entry.key.c_str(),
                                       entry.value.c_str(), snapshot.path.c_str());
    }
    void Run() {
        for (;;) {
            Snapshot snapshot;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                if (!pending_) return;
                snapshot = std::move(*pending_);
                pending_.reset();
            }
            // The previous synchronous code ignored individual profile-write
            // failures; keep that behavior while protecting the worker lifetime.
            try { write_(snapshot); } catch (...) {}
        }
    }

    Write write_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Snapshot> pending_;
    bool stopping_ = false;
    std::thread worker_;
};
} // namespace serialctl
