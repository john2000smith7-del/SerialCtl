#pragma once

#include "ByteTypes.h"
#include "TerminalDecoder.h"
#include "TerminalModel.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace serialctl {

// Fixed-size fair worker pool.  A Context is scheduled at most once, so all
// decode and VT state transitions for one session remain strictly ordered even
// when different sessions are parsed concurrently.
class TerminalParseScheduler {
public:
    struct Context;
    struct Update {
        std::shared_ptr<Context> context;
        std::uint64_t sessionId = 0;
        std::uint64_t sequence = 0;
        std::unique_ptr<TerminalModel> snapshot;
        TerminalFeedResult result;
        std::wstring decoded;
        std::wstring observedDecoded;
        std::wstring timestamp;
        std::int64_t arrival = 0;
        bool wasAlternateScreen = false;
        bool alternateScreen = false;
        std::uint64_t decodeUsec = 0;
        std::uint64_t parseUsec = 0;
        size_t inputBytes = 0;
        // false = terminal text, true = status line; retained in source order.
        std::vector<std::pair<bool, std::wstring>> logEntries;
    };
    using Publish = std::function<void(std::unique_ptr<Update>)>;
    using LogSink = std::function<void(const std::vector<std::pair<bool, std::wstring>>&)>;

    explicit TerminalParseScheduler(unsigned workerCount = 2);
    ~TerminalParseScheduler();
    TerminalParseScheduler(const TerminalParseScheduler&) = delete;
    TerminalParseScheduler& operator=(const TerminalParseScheduler&) = delete;

    void SetPublish(Publish publish);
    std::shared_ptr<Context> Create(std::uint64_t sessionId, const TerminalModel& initial,
                                    LogSink log = {});
    bool Enqueue(const std::shared_ptr<Context>&, Bytes data, unsigned codePage,
                 std::wstring timestamp, std::int64_t arrival = 0, bool observe = true);
    bool EnqueueText(const std::shared_ptr<Context>&, std::wstring text,
                     std::wstring timestamp, bool observe = false,
                     std::wstring statusAfter = {});
    std::uint64_t Resize(const std::shared_ptr<Context>&, int columns, int rows, bool wrap);
    std::uint64_t Clear(const std::shared_ptr<Context>&, bool scrollbackOnly = false);
    void Cancel(const std::shared_ptr<Context>&);
    void Stop();

    struct Statistics {
        size_t pendingBytes = 0;
        size_t highWaterBytes = 0;
        size_t pendingSessions = 0;
        std::uint64_t decodedBytes = 0;
        std::uint64_t updates = 0;
        std::uint64_t overflowEvents = 0;
    };
    Statistics Stats() const;

    struct Context {
    private:
        friend class TerminalParseScheduler;
        struct Job {
            enum class Kind { Bytes, Text, Resize, Clear, ClearScrollback } kind = Kind::Bytes;
            Bytes data;
            std::wstring text;
            unsigned codePage = 0;
            std::wstring timestamp;
            std::int64_t arrival = 0;
            std::uint64_t sequence = 0;
            bool observe = true;
            int columns = 0;
            int rows = 0;
            bool wrap = true;
            std::wstring statusAfter;
        };
        Context(std::uint64_t id, const TerminalModel& initial, LogSink sink)
            : sessionId(id), model(initial), log(std::move(sink)) {}
        const std::uint64_t sessionId;
        TerminalModel model;
        TerminalDecoder decoder;
        LogSink log;
        std::mutex mutex;
        std::deque<Job> jobs;
        size_t pendingBytes = 0;
        std::uint64_t nextSequence = 0;
        bool scheduled = false;
        bool canceled = false;
    };

private:
    void Worker();
    void ScheduleLocked(const std::shared_ptr<Context>&);

    const unsigned workerCount_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::shared_ptr<Context>> ready_;
    std::vector<std::thread> workers_;
    Publish publish_;
    bool stopping_ = false;
    size_t pendingBytes_ = 0;
    size_t highWaterBytes_ = 0;
    std::uint64_t decodedBytes_ = 0;
    std::uint64_t updates_ = 0;
    std::uint64_t overflowEvents_ = 0;
};

} // namespace serialctl
