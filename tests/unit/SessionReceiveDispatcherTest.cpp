#include "SessionReceiveDispatcher.h"
#include <iostream>
#include <cstdlib>
#define Check(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << "\n"; std::exit(1); } } while (false)
#include <string>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

using namespace serialctl;

#ifndef CP_UTF8
constexpr unsigned CP_UTF8 = 65001;
#endif

int main() {
    SessionReceiveDispatcher dispatcher;
    const auto begin = [&] { Check(dispatcher.BeginDrain()); };
    const auto finish = [&] { return dispatcher.FinishDrain(); };

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

    begin();
    auto batch = dispatcher.PopBatch(1, 10, std::chrono::milliseconds(10));
    Check(batch.size() == 1);
    Check(batch[0].sessionId == 1);
    Check(batch[0].data.size() == 10);
    Check(!dispatcher.HasPending());
    Check(dispatcher.PendingBytes(1) == 0);
    Check(!finish());

    // Test 3: Large chunking (> 16384 bytes)
    Bytes large(30000, 'X');
    dispatcher.Enqueue({2, large, CP_UTF8, "", 200}, needsPost);
    Check(needsPost);
    // Should be split into 16384 + 13616 (2 items)
    Check(dispatcher.PendingItemCount() == 2);
    Check(dispatcher.PendingBytes(2) == 30000);

    begin();
    auto largeBatch = dispatcher.PopBatch(2, 10, std::chrono::milliseconds(10));
    Check(largeBatch.size() == 2);
    Check(largeBatch[0].data.size() == 16384);
    Check(largeBatch[1].data.size() == 13616);
    Check(!dispatcher.HasPending());
    Check(!finish());

    // Test 4: Multi-session fairness (active session prioritized 3 out of 4)
    for (int i = 0; i < 20; ++i) {
        dispatcher.Enqueue({10, {'B'}, CP_UTF8, "fairness", 0}, needsPost); // background
        dispatcher.Enqueue({20, {'A'}, CP_UTF8, "fairness", 0}, needsPost); // active
    }
    begin();
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
    begin();
    auto prependedBatch = dispatcher.PopBatch(100, 10, std::chrono::milliseconds(10));
    Check(prependedBatch.size() == 3);
    Check(prependedBatch[0].data[0] == '1');
    Check(prependedBatch[1].data[0] == '2');
    Check(prependedBatch[2].data[0] == '3');
    Check(!finish());

    // Test 7: Overflow detection (> 4MB per session)
    Bytes huge(4 * 1024 * 1024 + 1024, 'O');
    dispatcher.Enqueue({999, huge, CP_UTF8, "", 0}, needsPost);
    auto overflow = dispatcher.TakeOverflow();
    Check(overflow.count(999) == 1);
    begin();
    dispatcher.PopBatch(999, 1000, std::chrono::milliseconds(100));
    Check(!finish());

    // Test 8: A failed post is re-armed without an empty/nonempty race.
    dispatcher.Enqueue({7, {'x'}, CP_UTF8, "", 0}, needsPost);
    Check(needsPost);
    Check(dispatcher.NotificationFailed());
    Check(dispatcher.Stats().notificationFailures == 1);
    begin();
    Check(dispatcher.PopBatch(7, 1, std::chrono::milliseconds(10)).size() == 1);
    Check(!finish());

    // Test 9: Concurrent producers and a sliced UI consumer preserve each
    // session's order while failures repeatedly transfer notification ownership.
    SessionReceiveDispatcher concurrent;
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::atomic<unsigned> signals{0};
    std::atomic<bool> producersDone{false};
    constexpr unsigned Producers = 4;
    constexpr unsigned PerProducer = 500;
    std::array<std::vector<unsigned>, Producers> observed;
    auto signal = [&](bool post) {
        if (!post) return;
        const auto attempt = signals.fetch_add(1) + 1;
        if (attempt % 11 == 0) {
            Check(concurrent.NotificationFailed());
        }
        wake.notify_one();
    };
    std::thread consumer([&] {
        while (!producersDone.load() || concurrent.HasPending()) {
            std::unique_lock<std::mutex> lock(wakeMutex);
            wake.wait_for(lock, std::chrono::milliseconds(10), [&] {
                return producersDone.load() || concurrent.HasPending();
            });
            lock.unlock();
            if (!concurrent.BeginDrain()) continue;
            auto items = concurrent.PopBatch(1, 7, std::chrono::milliseconds(1));
            for (const auto &item : items) {
                Check(item.sessionId >= 1 && item.sessionId <= Producers && item.data.size() == sizeof(unsigned));
                unsigned sequence = 0;
                std::memcpy(&sequence, item.data.data(), sizeof(sequence));
                observed[item.sessionId - 1].push_back(sequence);
            }
            if (concurrent.FinishDrain()) wake.notify_one();
        }
    });
    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < Producers; ++producer) {
        producers.emplace_back([&, producer] {
            for (unsigned sequence = 0; sequence < PerProducer; ++sequence) {
                Bytes value(sizeof(sequence));
                std::memcpy(value.data(), &sequence, sizeof(sequence));
                bool post = false;
                concurrent.Enqueue({producer + 1, std::move(value), CP_UTF8, "stress", 0}, post);
                signal(post);
                if ((sequence & 31) == 0) std::this_thread::yield();
            }
        });
    }
    for (auto &producer : producers) producer.join();
    producersDone = true;
    wake.notify_all();
    consumer.join();
    for (const auto &session : observed) {
        Check(session.size() == PerProducer);
        for (unsigned i = 0; i < PerProducer; ++i) Check(session[i] == i);
    }
    Check(concurrent.Stats().pendingBytes == 0);
    Check(concurrent.Stats().highWaterBytes > 0);

    // Test 10: Concurrent close rejects late producers and releases all data.
    SessionReceiveDispatcher closing;
    std::atomic<bool> stop{false};
    std::thread lateProducer([&] {
        while (!stop.load()) {
            bool post = false;
            closing.Enqueue({55, Bytes(64, 'z'), CP_UTF8, "close", 0}, post);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    closing.Close();
    stop = true;
    lateProducer.join();
    Check(!closing.HasPending());
    Check(closing.Stats().pendingBytes == 0);
    bool postAfterClose = true;
    closing.Enqueue({56, {'x'}, CP_UTF8, "reconnect", 0}, postAfterClose);
    Check(!postAfterClose && !closing.HasPending());

    std::cout << "SessionReceiveDispatcher tests passed successfully!\n";
    return 0;
}
