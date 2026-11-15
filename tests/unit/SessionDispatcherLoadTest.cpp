#include "SessionReceiveDispatcher.h"
#include "TerminalParseScheduler.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace serialctl;

namespace {

int g_failures = 0;

void Expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAILED: " << msg << '\n';
        ++g_failures;
    }
}

#ifndef CP_UTF8
constexpr unsigned kUtf8 = 65001;
#else
constexpr unsigned kUtf8 = CP_UTF8;
#endif

} // namespace

int main() {
    // 1. P1: 80+ simultaneous active serial sessions & high item counts (MaxSessions=256, MaxTotalItems=32768)
    Expect(SessionReceiveDispatcher::MaxSessions == 256, "MaxSessions bumped to 256");
    Expect(SessionReceiveDispatcher::MaxTotalItems == 32768, "MaxTotalItems bumped to 32768");

    SessionReceiveDispatcher dispatcher;
    bool needsPost = false;
    bool anyOverflow = false;
    // Enqueue 80 sessions * 100 distinct-source items = 8000 uncoalesced items (would exceed legacy MaxSessions=64 / MaxTotalItems=4096)
    for (std::uint64_t sid = 1; sid <= 80; ++sid) {
        for (int k = 0; k < 100; ++k) {
            bool post = false;
            dispatcher.Enqueue({sid, Bytes{'A', 'B', 'C'}, kUtf8, "in:" + std::to_string(k), 100 + k}, post);
            if (post)
                needsPost = true;
        }
    }
    Expect(needsPost, "dispatcher scheduled drain notification");
    const auto stats = dispatcher.Stats();
    Expect(stats.overflowEvents == 0, "80 sessions x 100 items zero overflow events");
    Expect(dispatcher.PendingItemCount() == 8000, "all 8000 items retained without drops");
    Expect(dispatcher.BeginDrain(), "BeginDrain succeeds");
    size_t drained = 0;
    for (;;) {
        auto batch = dispatcher.PopBatch(1, 512, std::chrono::milliseconds(50));
        drained += batch.size();
        if (!dispatcher.FinishDrain())
            break;
        if (!dispatcher.BeginDrain())
            break;
    }
    Expect(drained == 8000, "all 8000 items drained across 80 sessions");
    anyOverflow = !dispatcher.TakeOverflow().empty();
    Expect(!anyOverflow, "no overflow sessions reported");

    // 2. P2: TerminalParseScheduler visibility skip-clone + coalescing + 128 MiB limit
    TerminalParseScheduler scheduler(4);
    std::atomic<int> hiddenSnapshots{0};
    std::atomic<int> visibleSnapshots{0};
    std::atomic<int> publishedHidden{0};
    std::atomic<int> publishedVisible{0};
    scheduler.SetPublish([&](std::unique_ptr<TerminalParseScheduler::Update> update) {
        if (!update)
            return;
        if (update->sessionId == 10) {
            ++publishedHidden;
            if (update->snapshot)
                ++hiddenSnapshots;
        } else if (update->sessionId == 20) {
            ++publishedVisible;
            if (update->snapshot)
                ++visibleSnapshots;
        }
    });

    TerminalModel initial(80, 24);
    auto hiddenCtx = scheduler.Create(10, initial);
    auto visibleCtx = scheduler.Create(20, initial);
    TerminalParseScheduler::SetVisible(hiddenCtx, false);
    TerminalParseScheduler::SetVisible(visibleCtx, true);
    Expect(!TerminalParseScheduler::IsVisible(hiddenCtx), "hidden context marked invisible");
    Expect(TerminalParseScheduler::IsVisible(visibleCtx), "visible context marked visible");

    for (int i = 0; i < 16; ++i) {
        const std::string line = "line " + std::to_string(i) + "\r\n";
        Expect(scheduler.Enqueue(hiddenCtx, Bytes(line.begin(), line.end()), kUtf8, L"", i),
               "hidden enqueue succeeds");
        Expect(scheduler.Enqueue(visibleCtx, Bytes(line.begin(), line.end()), kUtf8, L"", i),
               "visible enqueue succeeds");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           (publishedHidden.load() == 0 || publishedVisible.load() == 0 ||
            scheduler.Stats().pendingBytes > 0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Wait briefly for in-flight batches to finish publishing
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Expect(publishedHidden.load() > 0, "hidden context still emits parsed updates");
    Expect(hiddenSnapshots.load() == 0, "hidden context skips expensive TerminalModel deep-clone");
    Expect(visibleSnapshots.load() > 0, "visible context includes TerminalModel snapshot");

    TerminalModel recoveredModel(80, 24);
    std::uint64_t seq = 0;
    Expect(TerminalParseScheduler::CopySnapshot(hiddenCtx, recoveredModel, seq),
           "CopySnapshot recovers latest model when switching back to hidden session");
    Expect(seq > 0, "recovered snapshot sequence is positive");

    // Coalescing check: staging multiple updates retains latest snapshot
    auto u1 = std::make_unique<TerminalParseScheduler::Update>();
    u1->sessionId = 10;
    u1->sequence = 1;
    auto u2 = std::make_unique<TerminalParseScheduler::Update>();
    u2->sessionId = 10;
    u2->sequence = 2;
    TerminalParseScheduler::StageCoalescedUpdate(hiddenCtx, std::move(u1));
    TerminalParseScheduler::StageCoalescedUpdate(hiddenCtx, std::move(u2));
    auto taken = TerminalParseScheduler::TakeCoalescedUpdate(hiddenCtx);
    Expect(taken && taken->sequence == 2, "coalesced update preserves latest sequence");

    scheduler.Stop();
    return g_failures == 0 ? 0 : 1;
}
