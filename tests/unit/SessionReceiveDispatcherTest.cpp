#include "SessionReceiveDispatcher.h"
#include <iostream>
#include <cstdlib>
#define Check(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << "\n"; std::exit(1); } } while (false)
#include <string>

using namespace serialctl;

int main() {
    SessionReceiveDispatcher dispatcher;

    // Test 1: Empty state
    Check(!dispatcher.HasPending());
    Check(dispatcher.PendingItemCount() == 0);
    Check(dispatcher.PendingBytes(1) == 0);

    // Test 2: Simple Enqueue and PopBatch FIFO
    bool needsPost = false;
    dispatcher.Enqueue({1, {'h', 'e', 'l', 'l', 'o'}, CP_UTF8, "", 100}, needsPost);
    Check(needsPost);
    Check(dispatcher.HasPending());
    Check(dispatcher.PendingItemCount() == 1);
    Check(dispatcher.PendingBytes(1) == 5);

    dispatcher.Enqueue({1, {'w', 'o', 'r', 'l', 'd'}, CP_UTF8, "", 101}, needsPost);
    // Coalesced into previous chunk because both are output from session 1 with same codePage
    Check(dispatcher.PendingItemCount() == 1);
    Check(dispatcher.PendingBytes(1) == 10);

    auto batch = dispatcher.PopBatch(1, 10, std::chrono::milliseconds(10));
    Check(batch.size() == 1);
    Check(batch[0].sessionId == 1);
    Check(batch[0].data.size() == 10);
    Check(!dispatcher.HasPending());
    Check(dispatcher.PendingBytes(1) == 0);

    // Test 3: Large chunking (> 16384 bytes)
    Bytes large(30000, 'X');
    dispatcher.Enqueue({2, large, CP_UTF8, "", 200}, needsPost);
    Check(needsPost);
    // Should be split into 16384 + 13616 (2 items)
    Check(dispatcher.PendingItemCount() == 2);
    Check(dispatcher.PendingBytes(2) == 30000);

    auto largeBatch = dispatcher.PopBatch(2, 10, std::chrono::milliseconds(10));
    Check(largeBatch.size() == 2);
    Check(largeBatch[0].data.size() == 16384);
    Check(largeBatch[1].data.size() == 13616);
    Check(!dispatcher.HasPending());

    // Test 4: Multi-session fairness (active session prioritized 3 out of 4)
    for (int i = 0; i < 20; ++i) {
        dispatcher.Enqueue({10, {'B'}, CP_UTF8, "", 0}, needsPost); // background
        dispatcher.Enqueue({20, {'A'}, CP_UTF8, "", 0}, needsPost); // active
    }
    auto fairBatch = dispatcher.PopBatch(20, 4, std::chrono::milliseconds(10));
    Check(fairBatch.size() == 4);
    int activeCount = 0;
    for (const auto &item : fairBatch) {
        if (item.sessionId == 20) activeCount++;
    }
    // 3 out of 4 opportunities given to active session
    Check(activeCount == 3);
    dispatcher.Clear();
    Check(!dispatcher.HasPending());

    // Test 5: Session removal
    dispatcher.Enqueue({5, {'a'}, CP_UTF8, "", 0}, needsPost);
    dispatcher.Enqueue({6, {'b'}, CP_UTF8, "", 0}, needsPost);
    Check(dispatcher.PendingBytes(5) == 1);
    Check(dispatcher.PendingBytes(6) == 1);
    dispatcher.RemoveSession(5);
    Check(dispatcher.PendingBytes(5) == 0);
    Check(dispatcher.PendingBytes(6) == 1);
    Check(dispatcher.PendingItemCount() == 1);
    dispatcher.Clear();

    // Test 6: PrependBatch
    std::vector<ReceivedItem> historyItems = {
        {100, {'1'}, CP_UTF8, "", 0},
        {100, {'2'}, CP_UTF8, "", 0}
    };
    dispatcher.Enqueue({100, {'3'}, CP_UTF8, "", 0}, needsPost);
    dispatcher.PrependBatch(historyItems, needsPost);
    auto prependedBatch = dispatcher.PopBatch(100, 10, std::chrono::milliseconds(10));
    Check(prependedBatch.size() == 3);
    Check(prependedBatch[0].data[0] == '1');
    Check(prependedBatch[1].data[0] == '2');
    Check(prependedBatch[2].data[0] == '3');

    // Test 7: Overflow detection (> 4MB per session)
    Bytes huge(4 * 1024 * 1024 + 1024, 'O');
    dispatcher.Enqueue({999, huge, CP_UTF8, "", 0}, needsPost);
    auto overflow = dispatcher.TakeOverflow();
    Check(overflow.count(999) == 1);

    std::cout << "SessionReceiveDispatcher tests passed successfully!\n";
    return 0;
}
