#pragma once
#include "Connection.h"
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <chrono>
#include <string>
#include <algorithm>

namespace serialctl {

struct ReceivedItem {
    std::uint64_t sessionId = 0;
    Bytes data;
    UINT codePage = 0;
    std::string source; // empty for output, nonempty for accepted input
    LONGLONG arrival = 0;
};

class SessionReceiveDispatcher {
public:
    static constexpr size_t MaxSessionBytes = 4 * 1024 * 1024;
    static constexpr size_t MaxTotalItems = 4096;
    static constexpr size_t MaxSessions = 64;
    static constexpr size_t MaxChunkSize = 16384;

    void Enqueue(ReceivedItem item, bool &needsPost) {
        if (item.source.empty() && item.data.size() > MaxChunkSize) {
            for (size_t offset = 0; offset < item.data.size(); offset += MaxChunkSize) {
                const auto end = std::min(offset + MaxChunkSize, item.data.size());
                EnqueueInternal({item.sessionId, Bytes(item.data.begin() + offset, item.data.begin() + end),
                                 item.codePage, {}, item.arrival}, needsPost);
            }
            return;
        }
        EnqueueInternal(std::move(item), needsPost);
    }

    void PrependBatch(std::vector<ReceivedItem> items, bool &needsPost) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto item = items.rbegin(); item != items.rend(); ++item) {
            receivedBytes_[item->sessionId] += item->data.size();
            received_.push_front(std::move(*item));
        }
        if (!received_.empty())
            needsPost = true;
    }

    std::vector<ReceivedItem> PopBatch(std::uint64_t activeSessionId, unsigned maxCount,
                                       std::chrono::milliseconds maxDuration) {
        std::vector<ReceivedItem> batch;
        const auto started = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        for (unsigned count = 0; count < maxCount && (count == 0 ||
             std::chrono::steady_clock::now() - started < maxDuration); ++count) {
            if (received_.empty())
                break;
            auto next = received_.begin();
            // Preserve order within each session while giving the visible
            // terminal three of every four opportunities during overload.
            if (activeSessionId != 0 && (++fairness_ % 4) != 0) {
                const auto visible = std::find_if(received_.begin(), received_.end(),
                    [activeSessionId](const ReceivedItem &entry) { return entry.sessionId == activeSessionId; });
                if (visible != received_.end())
                    next = visible;
            }
            auto item = std::move(*next);
            received_.erase(next);
            auto found = receivedBytes_.find(item.sessionId);
            if (found != receivedBytes_.end()) {
                found->second -= std::min(found->second, item.data.size());
                if (!found->second)
                    receivedBytes_.erase(found);
            }
            batch.push_back(std::move(item));
        }
        return batch;
    }

    std::set<std::uint64_t> TakeOverflow() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::set<std::uint64_t> overflow;
        overflow.swap(overflow_);
        return overflow;
    }

    void RemoveSession(std::uint64_t sessionId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = receivedBytes_.find(sessionId);
        if (it != receivedBytes_.end())
            receivedBytes_.erase(it);
        overflow_.erase(sessionId);
        for (auto iter = received_.begin(); iter != received_.end();) {
            if (iter->sessionId == sessionId)
                iter = received_.erase(iter);
            else
                ++iter;
        }
    }

    bool HasPending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !received_.empty() || !overflow_.empty();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        received_.clear();
        receivedBytes_.clear();
        overflow_.clear();
    }

    size_t PendingBytes(std::uint64_t sessionId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = receivedBytes_.find(sessionId);
        return it != receivedBytes_.end() ? it->second : 0;
    }

    size_t PendingItemCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_.size();
    }

    std::deque<ReceivedItem> &DirectItems() { return received_; }
    const std::deque<ReceivedItem> &DirectItems() const { return received_; }
    std::mutex &DirectMutex() { return mutex_; }
    unsigned &DirectFairness() { return fairness_; }

private:
    void EnqueueInternal(ReceivedItem item, bool &needsPost) {
        const auto id = item.sessionId;
        std::lock_guard<std::mutex> lock(mutex_);
        auto &bytes = receivedBytes_[id];
        if (bytes + item.data.size() > MaxSessionBytes || receivedBytes_.size() > MaxSessions ||
            received_.size() >= MaxTotalItems) {
            overflow_.insert(id);
        } else {
            bytes += item.data.size();
            if (!received_.empty() && item.source.empty() && received_.back().source.empty() &&
                received_.back().sessionId == id && received_.back().codePage == item.codePage &&
                received_.back().data.size() + item.data.size() <= MaxChunkSize) {
                received_.back().data.insert(received_.back().data.end(), item.data.begin(), item.data.end());
            } else {
                received_.push_back(std::move(item));
            }
        }
        needsPost = true;
    }

    mutable std::mutex mutex_;
    std::deque<ReceivedItem> received_;
    std::map<std::uint64_t, size_t> receivedBytes_;
    std::set<std::uint64_t> overflow_;
    unsigned fairness_ = 0;
};

} // namespace serialctl
