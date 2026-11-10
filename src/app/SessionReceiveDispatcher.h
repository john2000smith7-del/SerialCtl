#pragma once
#include "ByteTypes.h"
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
    unsigned codePage = 0;
    std::string source; // empty for output, nonempty for accepted input
    std::int64_t arrival = 0;
};

class SessionReceiveDispatcher {
public:
    static constexpr size_t MaxSessionBytes = 4 * 1024 * 1024;
    static constexpr size_t MaxTotalItems = 4096;
    static constexpr size_t MaxSessions = 64;
    static constexpr size_t MaxChunkSize = 16384;

    struct Statistics {
        size_t pendingBytes = 0;
        size_t pendingItems = 0;
        size_t highWaterBytes = 0;
        size_t highWaterItems = 0;
        std::uint64_t overflowEvents = 0;
        std::uint64_t notificationFailures = 0;
    };

    // needsPost is true only for the thread that changed the notification
    // state from Idle to Scheduled.  The caller must either deliver that
    // notification or call NotificationFailed().
    void Enqueue(ReceivedItem item, bool &needsPost) {
        needsPost = false;
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
        needsPost = false;
        if (closed_)
            return;
        for (auto item = items.rbegin(); item != items.rend(); ++item) {
            receivedBytes_[item->sessionId] += item->data.size();
            pendingBytes_ += item->data.size();
            received_.push_front(std::move(*item));
        }
        UpdateHighWaterLocked();
        needsPost = ArmNotificationLocked();
    }

    // Called by the UI before consuming the notification.  Duplicate or stale
    // messages are harmless and do not start a second drain.
    bool BeginDrain() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || notification_ != NotificationState::Scheduled)
            return false;
        notification_ = NotificationState::Draining;
        return true;
    }

    // Completes the atomic empty-check/notification handoff.  Producers that
    // enqueue while the UI is draining leave notification_ in Draining; this
    // method observes their data under the same lock and schedules the next
    // slice without losing the empty-to-nonempty edge.
    bool FinishDrain() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            notification_ = NotificationState::Idle;
            return false;
        }
        if (notification_ != NotificationState::Draining)
            return false;
        if (HasPendingLocked()) {
            notification_ = NotificationState::Scheduled;
            return true;
        }
        notification_ = NotificationState::Idle;
        return false;
    }

    // Re-arm after PostMessage/SetTimer failed.  A later producer will also
    // retry if the caller ultimately has to abandon this attempt.
    bool NotificationFailed(bool abandon = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++notificationFailures_;
        if (notification_ != NotificationState::Scheduled)
            return false;
        notification_ = NotificationState::Idle;
        return !abandon && ArmNotificationLocked();
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
            pendingBytes_ -= std::min(pendingBytes_, item.data.size());
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
        if (it != receivedBytes_.end()) {
            pendingBytes_ -= std::min(pendingBytes_, it->second);
            receivedBytes_.erase(it);
        }
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
        pendingBytes_ = 0;
        notification_ = NotificationState::Idle;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        received_.clear();
        receivedBytes_.clear();
        overflow_.clear();
        pendingBytes_ = 0;
        notification_ = NotificationState::Idle;
    }

    Statistics Stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {pendingBytes_, received_.size(), highWaterBytes_, highWaterItems_,
                overflowEvents_, notificationFailures_};
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

private:
#ifdef SERIALCTL_AI_TESTING
    friend struct AiDeviceTestAccess;
#endif
    void EnqueueInternal(ReceivedItem item, bool &needsPost) {
        const auto id = item.sessionId;
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return;
        auto &bytes = receivedBytes_[id];
        if (bytes + item.data.size() > MaxSessionBytes || receivedBytes_.size() > MaxSessions ||
            received_.size() >= MaxTotalItems) {
            overflow_.insert(id);
            ++overflowEvents_;
        } else {
            bytes += item.data.size();
            pendingBytes_ += item.data.size();
            if (!received_.empty() && item.source.empty() && received_.back().source.empty() &&
                received_.back().sessionId == id && received_.back().codePage == item.codePage &&
                received_.back().data.size() + item.data.size() <= MaxChunkSize) {
                received_.back().data.insert(received_.back().data.end(), item.data.begin(), item.data.end());
            } else {
                received_.push_back(std::move(item));
            }
        }
        UpdateHighWaterLocked();
        needsPost = ArmNotificationLocked() || needsPost;
    }

    enum class NotificationState { Idle, Scheduled, Draining };
    bool HasPendingLocked() const { return !received_.empty() || !overflow_.empty(); }
    bool ArmNotificationLocked() {
        if (closed_ || !HasPendingLocked() || notification_ != NotificationState::Idle)
            return false;
        notification_ = NotificationState::Scheduled;
        return true;
    }
    void UpdateHighWaterLocked() {
        highWaterBytes_ = std::max(highWaterBytes_, pendingBytes_);
        highWaterItems_ = std::max(highWaterItems_, received_.size());
    }

    mutable std::mutex mutex_;
    std::deque<ReceivedItem> received_;
    std::map<std::uint64_t, size_t> receivedBytes_;
    std::set<std::uint64_t> overflow_;
    unsigned fairness_ = 0;
    size_t pendingBytes_ = 0;
    size_t highWaterBytes_ = 0;
    size_t highWaterItems_ = 0;
    std::uint64_t overflowEvents_ = 0;
    std::uint64_t notificationFailures_ = 0;
    NotificationState notification_ = NotificationState::Idle;
    bool closed_ = false;
};

} // namespace serialctl
