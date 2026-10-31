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
        std::string state = "idle", pattern, prompt, error, preset;
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
        continuous_ = false;
        bytes_ = std::move(bytes);
        tail_.clear();
        tailStartsLine_ = true;
        deadline_ = now + seconds * 1000ull;
        nextSend_ = 0;
        return true;
    }
    // Two built-in strategies. Prepare before queuing reboot; no interrupt
    // bytes may precede an accepted restart command on that same transport.
    bool PreparePreset(const std::string& preset, unsigned seconds, std::uint64_t now) {
        if ((preset != "uboot_space" && preset != "phytium_uefi_f2") || seconds < 1 || seconds > 120) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (Active()) return false;
        status_ = {};
        status_.state = "prepared";
        status_.preset = preset;
        status_.prompt = preset == "uboot_space" ? "=>" : "UEFI setup/boot menu";
        bytes_ = preset == "uboot_space" ? " " : "\x1bOQ"; // VT100/VT100+ F2, never the literal text F2.
        continuous_ = true;
        tail_.clear(); tailStartsLine_ = true;
        deadline_ = now + seconds * 1000ull;
        nextSend_ = now;
        return true;
    }
    void RestartSubmitted(std::uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != "prepared") return;
        status_.state = "interrupting";
        nextSend_ = now;
        Recognize();
    }
    std::string Feed(const char *data, size_t size, std::uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Active() || now >= deadline_) return Step(now);
        // Match across receive chunks while keeping a fixed bounded tail.
        for (size_t offset = 0; offset < size; offset += 512) {
            tail_.append(data + offset, std::min<size_t>(512, size - offset));
            if (status_.state != "prepared" && Recognize()) return {};
            if (!continuous_ && !status_.matched && tail_.find(status_.pattern) != std::string::npos) {
                status_.matched = true;
                status_.state = "interrupting";
                nextSend_ = now;
            }
            if (tail_.size() > 1024) {
                // After trimming, offset 0 is a real line start only if the dropped
                // byte before it was a line terminator.
                const size_t drop = tail_.size() - 1024;
                tailStartsLine_ = tail_[drop - 1] == '\r' || tail_[drop - 1] == '\n';
                tail_.erase(0, drop);
            }
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
    bool Active() const { return status_.state == "prepared" || status_.state == "armed" || status_.state == "interrupting"; }
    bool Recognize() {
        bool found = false;
        if (status_.preset == "phytium_uefi_f2") {
            // A vendor/UEFI banner or "Press F2" instruction is not entry.
            // Require actual, distinctive menu content or a fresh shell prompt.
            found = (tail_.find("Boot Manager") != std::string::npos &&
                     tail_.find("Boot Maintenance Manager") != std::string::npos) ||
                    (tail_.find("Setup Utility") != std::string::npos && tail_.find("Main") != std::string::npos &&
                     tail_.find("Advanced") != std::string::npos && tail_.find("Save & Exit") != std::string::npos);
        } else {
            const auto at = tail_.rfind(status_.prompt);
            found = at != std::string::npos && ((at == 0 && tailStartsLine_) ||
                (at > 0 && (tail_[at-1] == '\r' || tail_[at-1] == '\n'))) &&
                (at + status_.prompt.size() == tail_.size() || tail_[at+status_.prompt.size()] == ' ');
        }
        if (found) { status_.state = "entered"; status_.entered = true; }
        return found;
    }
    std::string Step(std::uint64_t now) {
        if (!Active()) return {};
        if (now >= deadline_) { status_.state = "timed_out"; return {}; }
        if (status_.state == "prepared" || (!continuous_ && (!status_.matched || status_.submitted >= 3)) || now < nextSend_) return {};
        ++status_.submitted;
        nextSend_ = now + 80;
        return bytes_;
    }
    mutable std::mutex mutex_;
    Snapshot status_;
    std::string bytes_, tail_;
    bool tailStartsLine_ = true;
    bool continuous_ = false;
    std::uint64_t deadline_ = 0, nextSend_ = 0;
};
} // namespace serialctl
