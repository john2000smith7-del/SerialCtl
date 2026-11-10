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
            auto& queue = received_[item->sessionId];
            if (queue.empty()) readySessions_.push_front(item->sessionId);
            receivedBytes_[item->sessionId] += item->data.size();
            pendingBytes_ += item->data.size();
            ++pendingItems_;
            queue.push_front(std::move(*item));
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
            if (readySessions_.empty())
                break;
            std::uint64_t selected = readySessions_.front();
            const bool preferVisible = activeSessionId && (++fairness_ % 4) != 0;
            const auto visible = received_.find(activeSessionId);
            if (preferVisible && visible != received_.end() && !visible->second.empty()) {
                selected = activeSessionId;
            } else if (!preferVisible && selected == activeSessionId && readySessions_.size() > 1) {
                const auto background = std::find_if(readySessions_.begin(), readySessions_.end(),
                    [activeSessionId](std::uint64_t id) { return id != activeSessionId; });
                if (background != readySessions_.end()) selected = *background;
            }
            const auto order = std::find(readySessions_.begin(), readySessions_.end(), selected);
            if (order != readySessions_.end()) readySessions_.erase(order);
            auto session = received_.find(selected);
            if (session == received_.end() || session->second.empty()) continue;
            auto item = std::move(session->second.front());
            session->second.pop_front();
            --pendingItems_;
            pendingBytes_ -= std::min(pendingBytes_, item.data.size());
            auto found = receivedBytes_.find(item.sessionId);
            if (found != receivedBytes_.end()) {
                found->second -= std::min(found->second, item.data.size());
                if (!found->second)
                    receivedBytes_.erase(found);
            }
            if (!session->second.empty())
                readySessions_.push_back(selected);
            else
                received_.erase(session);
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
        const auto queue = received_.find(sessionId);
        if (queue != received_.end()) {
            pendingItems_ -= std::min(pendingItems_, queue->second.size());
            received_.erase(queue);
        }
        readySessions_.erase(std::remove(readySessions_.begin(), readySessions_.end(), sessionId),
                             readySessions_.end());
    }

    bool HasPending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pendingItems_ != 0 || !overflow_.empty();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        received_.clear();
        readySessions_.clear();
        receivedBytes_.clear();
        overflow_.clear();
        pendingBytes_ = 0;
        pendingItems_ = 0;
        notification_ = NotificationState::Idle;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        received_.clear();
        readySessions_.clear();
        receivedBytes_.clear();
        overflow_.clear();
        pendingBytes_ = 0;
        pendingItems_ = 0;
        notification_ = NotificationState::Idle;
    }

    Statistics Stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {pendingBytes_, pendingItems_, highWaterBytes_, highWaterItems_,
                overflowEvents_, notificationFailures_};
    }

    size_t PendingBytes(std::uint64_t sessionId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = receivedBytes_.find(sessionId);
        return it != receivedBytes_.end() ? it->second : 0;
    }

    size_t PendingItemCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pendingItems_;
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
        const auto byteEntry = receivedBytes_.find(id);
        const size_t bytes = byteEntry == receivedBytes_.end() ? 0 : byteEntry->second;
        const bool newSession = byteEntry == receivedBytes_.end();
        if (bytes + item.data.size() > MaxSessionBytes ||
            (newSession && receivedBytes_.size() >= MaxSessions) || pendingItems_ >= MaxTotalItems) {
            overflow_.insert(id);
            ++overflowEvents_;
        } else {
            receivedBytes_[id] = bytes + item.data.size();
            pendingBytes_ += item.data.size();
            auto& queue = received_[id];
            if (!queue.empty() && item.source.empty() && queue.back().source.empty() &&
                queue.back().codePage == item.codePage &&
                queue.back().data.size() + item.data.size() <= MaxChunkSize) {
                queue.back().data.insert(queue.back().data.end(), item.data.begin(), item.data.end());
            } else {
                if (queue.empty()) readySessions_.push_back(id);
                queue.push_back(std::move(item));
                ++pendingItems_;
            }
        }
        UpdateHighWaterLocked();
        needsPost = ArmNotificationLocked() || needsPost;
    }

    enum class NotificationState { Idle, Scheduled, Draining };
    bool HasPendingLocked() const { return pendingItems_ != 0 || !overflow_.empty(); }
    bool ArmNotificationLocked() {
        if (closed_ || !HasPendingLocked() || notification_ != NotificationState::Idle)
            return false;
        notification_ = NotificationState::Scheduled;
        return true;
    }
    void UpdateHighWaterLocked() {
        highWaterBytes_ = std::max(highWaterBytes_, pendingBytes_);
        highWaterItems_ = std::max(highWaterItems_, pendingItems_);
    }

    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::deque<ReceivedItem>> received_;
    std::deque<std::uint64_t> readySessions_;
    std::map<std::uint64_t, size_t> receivedBytes_;
    std::set<std::uint64_t> overflow_;
    unsigned fairness_ = 0;
    size_t pendingBytes_ = 0;
    size_t pendingItems_ = 0;
    size_t highWaterBytes_ = 0;
    size_t highWaterItems_ = 0;
    std::uint64_t overflowEvents_ = 0;
    std::uint64_t notificationFailures_ = 0;
    NotificationState notification_ = NotificationState::Idle;
    bool closed_ = false;
};

} // namespace serialctl
