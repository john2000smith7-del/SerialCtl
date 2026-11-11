#include "TerminalParseScheduler.h"

#include <algorithm>
#include <chrono>

namespace serialctl {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t MaximumBatchBytes = 64 * 1024;
constexpr size_t MaximumBatchItems = 32;
constexpr size_t MaximumContextBytes = 4 * 1024 * 1024;
constexpr size_t MaximumTotalBytes = 16 * 1024 * 1024;

void MergeResult(TerminalFeedResult& destination, TerminalFeedResult source) {
    destination.response += source.response;
    destination.logText += source.logText;
    destination.scrollbackAdded += source.scrollbackAdded;
    if (!source.workingDirectory.empty()) destination.workingDirectory = std::move(source.workingDirectory);
    if (source.titleChanged) {
        destination.titleChanged = true;
        destination.title = std::move(source.title);
    }
}
} // namespace

TerminalParseScheduler::TerminalParseScheduler(unsigned workerCount)
    : workerCount_((std::max)(1u, (std::min)(workerCount, 4u))) {
    workers_.reserve(workerCount_);
    for (unsigned i = 0; i < workerCount_; ++i)
        workers_.emplace_back([this] { Worker(); });
}

TerminalParseScheduler::~TerminalParseScheduler() { Stop(); }

void TerminalParseScheduler::SetPublish(Publish publish) {
    std::lock_guard<std::mutex> lock(mutex_);
    publish_ = std::move(publish);
}

std::shared_ptr<TerminalParseScheduler::Context> TerminalParseScheduler::Create(
    std::uint64_t sessionId, const TerminalModel& initial, LogSink log) {
    return std::shared_ptr<Context>(new Context(sessionId, initial, std::move(log)));
}

bool TerminalParseScheduler::Enqueue(const std::shared_ptr<Context>& context, Bytes data,
                                     unsigned codePage, std::wstring timestamp, std::int64_t arrival,
                                     bool observe) {
    if (!context || data.empty()) return false;
    const size_t bytes = data.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> contextLock(context->mutex);
        if (stopping_ || context->canceled) return false;
        if (context->pendingBytes + bytes > MaximumContextBytes || pendingBytes_ + bytes > MaximumTotalBytes) {
            ++overflowEvents_;
            return false;
        }
        context->pendingBytes += bytes;
        Context::Job job;
        job.kind = Context::Job::Kind::Bytes;
        job.data = std::move(data);
        job.codePage = codePage;
        job.timestamp = std::move(timestamp);
        job.arrival = arrival;
        job.sequence = ++context->nextSequence;
        job.observe = observe;
        context->jobs.push_back(std::move(job));
        pendingBytes_ += bytes;
        highWaterBytes_ = (std::max)(highWaterBytes_, pendingBytes_);
        if (!context->scheduled) {
            context->scheduled = true;
            ready_.push_back(context);
        }
    }
    wake_.notify_one();
    return true;
}

bool TerminalParseScheduler::EnqueueText(const std::shared_ptr<Context>& context, std::wstring text,
                                         std::wstring timestamp, bool observe, std::wstring statusAfter) {
    if (!context || text.empty()) return false;
    const size_t bytes = text.size() * sizeof(wchar_t);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> contextLock(context->mutex);
        if (stopping_ || context->canceled) return false;
        if (context->pendingBytes + bytes > MaximumContextBytes || pendingBytes_ + bytes > MaximumTotalBytes) {
            ++overflowEvents_;
            return false;
        }
        context->pendingBytes += bytes;
        Context::Job job;
        job.kind = Context::Job::Kind::Text;
        job.text = std::move(text);
        job.timestamp = std::move(timestamp);
        job.sequence = ++context->nextSequence;
        job.observe = observe;
        job.statusAfter = std::move(statusAfter);
        context->jobs.push_back(std::move(job));
        pendingBytes_ += bytes;
        highWaterBytes_ = (std::max)(highWaterBytes_, pendingBytes_);
        if (!context->scheduled) {
            context->scheduled = true;
            ready_.push_back(context);
        }
    }
    wake_.notify_one();
    return true;
}

std::uint64_t TerminalParseScheduler::Resize(const std::shared_ptr<Context>& context,
                                             int columns, int rows, bool wrap) {
    if (!context) return 0;
    std::uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> contextLock(context->mutex);
        if (stopping_ || context->canceled) return 0;
        Context::Job job;
        job.kind = Context::Job::Kind::Resize;
        job.columns = columns;
        job.rows = rows;
        job.wrap = wrap;
        sequence = job.sequence = ++context->nextSequence;
        context->jobs.push_back(std::move(job));
        if (!context->scheduled) {
            context->scheduled = true;
            ready_.push_back(context);
        }
    }
    wake_.notify_one();
    return sequence;
}

std::uint64_t TerminalParseScheduler::Clear(const std::shared_ptr<Context>& context, bool scrollbackOnly) {
    if (!context) return 0;
    std::uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> contextLock(context->mutex);
        if (stopping_ || context->canceled) return 0;
        Context::Job job;
        job.kind = scrollbackOnly ? Context::Job::Kind::ClearScrollback : Context::Job::Kind::Clear;
        sequence = job.sequence = ++context->nextSequence;
        context->jobs.push_back(std::move(job));
        if (!context->scheduled) {
            context->scheduled = true;
            ready_.push_back(context);
        }
    }
    wake_.notify_one();
    return sequence;
}

void TerminalParseScheduler::Cancel(const std::shared_ptr<Context>& context) {
    if (!context) return;
    size_t discarded = 0;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->canceled = true;
        discarded = context->pendingBytes;
        context->pendingBytes = 0;
        context->jobs.clear();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pendingBytes_ -= (std::min)(pendingBytes_, discarded);
}

void TerminalParseScheduler::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && workers_.empty()) return;
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    ready_.clear();
    pendingBytes_ = 0;
}

TerminalParseScheduler::Statistics TerminalParseScheduler::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {pendingBytes_, highWaterBytes_, ready_.size(), decodedBytes_, updates_, overflowEvents_};
}

void TerminalParseScheduler::Worker() {
    for (;;) {
        std::shared_ptr<Context> context;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !ready_.empty(); });
            if (ready_.empty()) {
                if (stopping_) return;
                continue;
            }
            context = std::move(ready_.front());
            ready_.pop_front();
        }

        std::vector<Context::Job> jobs;
        size_t batchBytes = 0;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            if (!context->canceled) {
                while (!context->jobs.empty() && jobs.size() < MaximumBatchItems &&
                       (jobs.empty() || batchBytes + context->jobs.front().data.size() +
                            context->jobs.front().text.size() * sizeof(wchar_t) <= MaximumBatchBytes)) {
                    batchBytes += context->jobs.front().data.size() +
                        context->jobs.front().text.size() * sizeof(wchar_t);
                    jobs.push_back(std::move(context->jobs.front()));
                    context->jobs.pop_front();
                }
                context->pendingBytes -= (std::min)(context->pendingBytes, batchBytes);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingBytes_ -= (std::min)(pendingBytes_, batchBytes);
        }

        std::unique_ptr<Update> update;
        if (!jobs.empty()) {
            update = std::make_unique<Update>();
            update->context = context;
            update->sessionId = context->sessionId;
            update->wasAlternateScreen = context->model.AlternateScreen();
            for (auto& job : jobs) {
                if (job.kind == Context::Job::Kind::Resize) {
                    context->model.SetLineWrapView(job.wrap);
                    context->model.Resize(job.columns, job.rows);
                    update->sequence = job.sequence;
                    continue;
                }
                if (job.kind == Context::Job::Kind::Clear ||
                    job.kind == Context::Job::Kind::ClearScrollback) {
                    if (job.kind == Context::Job::Kind::Clear) context->model.Clear();
                    else context->model.ClearScrollback();
                    update->sequence = job.sequence;
                    continue;
                }
                std::wstring decoded;
                if (job.kind == Context::Job::Kind::Text) {
                    decoded = std::move(job.text);
                } else {
                    const auto decodeStart = Clock::now();
                    decoded = context->decoder.Decode(job.data, job.codePage);
                    update->decodeUsec += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - decodeStart).count());
                }
                const auto parseStart = Clock::now();
                auto feedResult = context->model.Feed(decoded, job.timestamp);
                update->parseUsec += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - parseStart).count());
                if (!feedResult.logText.empty()) update->logEntries.push_back({false, feedResult.logText});
                if (!job.statusAfter.empty()) update->logEntries.push_back({true, std::move(job.statusAfter)});
                MergeResult(update->result, std::move(feedResult));
                update->decoded += decoded;
                if (job.observe) update->observedDecoded += decoded;
                if (!job.timestamp.empty()) update->timestamp = job.timestamp;
                if (job.arrival) update->arrival = job.arrival;
                update->inputBytes += job.data.size();
                update->sequence = job.sequence;
            }
            update->alternateScreen = context->model.AlternateScreen();
            // TerminalModel history entries are shared immutable lines.  This
            // copies the small live grids and shared_ptr index, never 20,000
            // historical cell buffers; UI ownership is transferred by move.
            update->snapshot = std::make_unique<TerminalModel>(context->model);
            if (context->log && !update->logEntries.empty())
                context->log(update->logEntries);
        }

        bool requeue = false;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            if (!context->canceled && !context->jobs.empty())
                requeue = true;
            else
                context->scheduled = false;
        }
        Publish publish;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (update) {
                decodedBytes_ += update->inputBytes;
                ++updates_;
            }
            publish = publish_;
        }
        if (update && publish) publish(std::move(update));
        // Publish before another worker may process this context, preserving
        // completion and logger order even when contexts migrate in the pool.
        if (requeue) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ready_.push_back(context);
            }
            wake_.notify_one();
        }
    }
}

} // namespace serialctl
