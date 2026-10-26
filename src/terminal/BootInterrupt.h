#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>

namespace serialctl {
// Runs next to the receiving transport, before UI/model/network round trips.
// Bytes returned here are only submissions; success requires a fresh prompt.
class BootInterrupt {
public:
    struct Snapshot {
        std::string state = "idle", pattern, prompt, error;
        unsigned submitted = 0;
        bool matched = false, entered = false;
        std::uint64_t remainingMs = 0;
    };
    bool Arm(std::string pattern, std::string bytes, std::string prompt, unsigned seconds, std::uint64_t now) {
        const auto printable = [](const std::string &s) {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 32 && c < 127; });
        };
        if (!printable(pattern) || pattern.size() > 128 || !printable(bytes) || bytes.size() > 32 ||
            !printable(prompt) || prompt.size() > 128 || seconds < 1 || seconds > 120) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = {};
        status_.state = "armed";
        status_.pattern = std::move(pattern);
        status_.prompt = std::move(prompt);
        bytes_ = std::move(bytes);
        tail_.clear();
        deadline_ = now + seconds * 1000ull;
        nextSend_ = 0;
        return true;
    }
    std::string Feed(const char *data, size_t size, std::uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Active() || now >= deadline_) return Step(now);
        // Match across receive chunks while keeping a fixed bounded tail.
        for (size_t offset = 0; offset < size; offset += 512) {
            tail_.append(data + offset, std::min<size_t>(512, size - offset));
            const auto at = tail_.rfind(status_.prompt);
            if (at != std::string::npos && (at == 0 || tail_[at - 1] == '\r' || tail_[at - 1] == '\n') &&
                (at + status_.prompt.size() == tail_.size() || tail_[at + status_.prompt.size()] == ' ')) {
                status_.state = "entered";
                status_.entered = true;
                return {};
            }
            if (!status_.matched && tail_.find(status_.pattern) != std::string::npos) {
                status_.matched = true;
                status_.state = "interrupting";
                nextSend_ = now;
            }
            if (tail_.size() > 1024) tail_.erase(0, tail_.size() - 1024);
        }
        return Step(now);
    }
    std::string Poll(std::uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        return Step(now);
    }
    void Delivery(bool accepted, std::string error) {
        if (accepted) return;
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = "failed";
        status_.error = std::move(error);
    }
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (Active()) status_.state = "canceled";
    }
    Snapshot Status(std::uint64_t now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot result = status_;
        result.remainingMs = Active() && now < deadline_ ? deadline_ - now : 0;
        return result;
    }
private:
    bool Active() const { return status_.state == "armed" || status_.state == "interrupting"; }
    std::string Step(std::uint64_t now) {
        if (!Active()) return {};
        if (now >= deadline_) { status_.state = "timed_out"; return {}; }
        if (!status_.matched || status_.submitted >= 3 || now < nextSend_) return {};
        ++status_.submitted;
        nextSend_ = now + 80;
        return bytes_;
    }
    mutable std::mutex mutex_;
    Snapshot status_;
    std::string bytes_, tail_;
    std::uint64_t deadline_ = 0, nextSend_ = 0;
};
} // namespace serialctl
