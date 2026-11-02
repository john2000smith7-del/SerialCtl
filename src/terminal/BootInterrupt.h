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
    struct Burst {std::string bytes;std::uint64_t generation=0;};
    struct Snapshot {
        std::string state = "idle", pattern, prompt, error, preset;
        unsigned submitted = 0, intervalMs = 80;
        bool matched = false, entered = false, untilStopped = false;
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
        ++generation_;
        status_.pattern = std::move(pattern);
        status_.prompt = std::move(prompt);
        continuous_ = false;
        bytes_ = std::move(bytes);
        tail_.clear();
        tailStartsLine_ = true;
        escapeState_ = 0;
        deadline_ = now + seconds * 1000ull;
        nextSend_ = 0;
        return true;
    }
    // Two built-in strategies. Prepare before queuing reboot; no interrupt
    // bytes may precede an accepted restart command on that same transport.
    bool PreparePreset(const std::string& preset, unsigned seconds, std::uint64_t now, unsigned intervalMs = 300) {
        const bool uboot=preset=="uboot_stop"||preset=="uboot_space";
        if ((!uboot && preset != "phytium_uefi_f2") || seconds > 120 ||
            (intervalMs != 300 && intervalMs != 500)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (Active()) return false;
        status_ = {};
        status_.state = "prepared";
        ++generation_;
        status_.preset = uboot ? "uboot_stop" : preset;
        status_.intervalMs = uboot ? 500 : intervalMs;
        status_.prompt = uboot ? "=>" : "UEFI setup/boot menu";
        bytes_ = uboot ? " stop" : "\x1bOQ";
        continuous_ = true;
        tail_.clear(); tailStartsLine_ = true;
        escapeState_ = 0;
        deadline_ = seconds ? now + seconds * 1000ull : 0;
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
    Burst FeedBurst(const char *data, size_t size, std::uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Active() || (deadline_ && now >= deadline_)) return {Step(now),generation_};
        // Match across receive chunks while keeping a fixed bounded tail.
        for (size_t offset = 0; offset < size; offset += 512) {
            for(size_t i=offset;i<std::min(size,offset+512);++i) {
                const auto c=static_cast<unsigned char>(data[i]);
                if(escapeState_==1) {escapeState_=c=='['?2:c==']'?3:(c=='('||c==')')?5:0;continue;}
                if(escapeState_==2) {if(c>=0x40&&c<=0x7e)escapeState_=0;continue;}
                if(escapeState_==3) {if(c==7)escapeState_=0;else if(c==27)escapeState_=4;continue;}
                if(escapeState_==4) {escapeState_=c=='\\'?0:3;continue;}
                if(escapeState_==5) {escapeState_=0;continue;}
                if(c==27) {escapeState_=1;continue;}
                if(c=='\b'||c==127) {if(!tail_.empty()&&tail_.back()!='\r'&&tail_.back()!='\n')tail_.pop_back();continue;}
                if(c>=32||c=='\r'||c=='\n')tail_.push_back(static_cast<char>(c));
                else if(c=='\t')tail_.push_back(' ');
            }
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
        return {Step(now),generation_};
    }
    std::string Feed(const char* data,size_t size,std::uint64_t now) {return FeedBurst(data,size,now).bytes;}
    Burst PollBurst(std::uint64_t now) {std::lock_guard<std::mutex> lock(mutex_);return {Step(now),generation_};}
    std::string Poll(std::uint64_t now) {
        return PollBurst(now).bytes;
    }
    bool CanSend(std::uint64_t generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation==generation_&&status_.state=="interrupting";
    }
    void Delivery(bool accepted, std::string error) {
        if (accepted) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if(!Active())return;
        status_.state = "failed";
        status_.error = std::move(error);
    }
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (Active()) {status_.state = "canceled";++generation_;}
    }
    Snapshot Status(std::uint64_t now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot result = status_;
        result.remainingMs = Active() && deadline_ && now < deadline_ ? deadline_ - now : 0;
        result.untilStopped = continuous_ && !deadline_;
        return result;
    }
private:
    bool Active() const { return status_.state == "prepared" || status_.state == "armed" || status_.state == "interrupting"; }
    bool HasPrompt(const std::string& prompt) const {
        for(auto at=tail_.find(prompt);at!=std::string::npos;at=tail_.find(prompt,at+1)) {
            auto start=at;while(start&&(tail_[start-1]==' '||tail_[start-1]=='\t'))--start;
            const auto end=at+prompt.size();
            if(((start==0&&tailStartsLine_)||(start>0&&(tail_[start-1]=='\r'||tail_[start-1]=='\n')))&&
               (end==tail_.size()||tail_[end]==' '||tail_[end]=='\t'||tail_[end]=='\r'||tail_[end]=='\n'))return true;
        }
        return false;
    }
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
            found=HasPrompt(status_.prompt);
            if(!found&&status_.preset=="uboot_stop") {
                // Match a real line prompt, including ANSI-colored prompts and
                // named U-Boot consoles. Banners/countdown prose are not entry.
                for(const auto* prompt:{"U-Boot>","U-Boot#","U-Boot #","hisilicon #","HiSilicon #"}) {
                    const std::string candidate=prompt;
                    if(HasPrompt(candidate)) {
                        found=true;status_.prompt=candidate;break;
                    }
                }
            }
        }
        if (found) { status_.state = "entered"; status_.entered = true; }
        return found;
    }
    std::string Step(std::uint64_t now) {
        if (!Active()) return {};
        if (deadline_ && now >= deadline_) { status_.state = "timed_out"; return {}; }
        if (status_.state == "prepared" || (!continuous_ && (!status_.matched || status_.submitted >= 3)) || now < nextSend_) return {};
        ++status_.submitted;
        nextSend_ = now + status_.intervalMs;
        return bytes_;
    }
    mutable std::mutex mutex_;
    Snapshot status_;
    std::string bytes_, tail_;
    bool tailStartsLine_ = true;
    bool continuous_ = false;
    std::uint64_t deadline_ = 0, nextSend_ = 0;
    std::uint64_t generation_ = 0;
    int escapeState_ = 0;
};
} // namespace serialctl
