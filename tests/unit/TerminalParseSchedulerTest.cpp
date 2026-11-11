#include "TerminalParseScheduler.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

#define Check(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << "\n"; std::exit(1); } } while (false)

using namespace serialctl;

int main() {
    TerminalParseScheduler scheduler(2);
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::uint64_t, std::unique_ptr<TerminalParseScheduler::Update>> latest;
    scheduler.SetPublish([&](std::unique_ptr<TerminalParseScheduler::Update> update) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            latest[update->sessionId] = std::move(update);
        }
        changed.notify_all();
    });
    auto first = scheduler.Create(1, TerminalModel(80, 24));
    auto second = scheduler.Create(2, TerminalModel(80, 24));
    for (int i = 0; i < 100; ++i) {
        const auto line = std::to_string(i) + "\r\n";
        Check(scheduler.Enqueue(first, Bytes(line.begin(), line.end()), CP_UTF8, L""));
        const std::string colored = "\x1b[3" + std::to_string(i % 8) + "mB" + std::to_string(i) + "\x1b[0m\r\n";
        Check(scheduler.Enqueue(second, Bytes(colored.begin(), colored.end()), CP_UTF8, L""));
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return latest.count(1) && latest.count(2) && latest[1]->sequence == 100 && latest[2]->sequence == 100;
        }));
        const auto firstText = latest[1]->snapshot->PlainText(false);
        const auto secondText = latest[2]->snapshot->PlainText(false);
        Check(firstText.find(L"0\r\n1\r\n2") != std::wstring::npos);
        Check(firstText.find(L"98\r\n99") != std::wstring::npos);
        Check(secondText.find(L"B0") != std::wstring::npos && secondText.find(L"B99") != std::wstring::npos);
    }

    const auto clearSequence = scheduler.Clear(first);
    Check(clearSequence > 100);
    Check(scheduler.EnqueueText(first, L"after clear\r\n", L"", false, L"input status"));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return latest.count(1) && latest[1]->sequence > clearSequence;
        }));
        Check(latest[1]->snapshot->PlainText(false).find(L"after clear") != std::wstring::npos);
        Check(latest[1]->snapshot->PlainText(false).find(L"98") == std::wstring::npos);
        Check(latest[1]->logEntries.size() == 2 && !latest[1]->logEntries[0].first &&
              latest[1]->logEntries[1].first && latest[1]->logEntries[1].second == L"input status");
    }

    const auto resizeSequence = scheduler.Resize(first, 132, 43, false);
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return latest.count(1) && latest[1]->sequence >= resizeSequence;
        }));
        Check(latest[1]->snapshot->Columns() == 132 && latest[1]->snapshot->Rows() == 43 &&
              !latest[1]->snapshot->LineWrapView());
    }

    Check(!scheduler.Enqueue(first, Bytes(4 * 1024 * 1024 + 1, 'x'), CP_UTF8, L""));
    Check(scheduler.Stats().overflowEvents == 1);
    scheduler.Cancel(second);
    Check(!scheduler.Enqueue(second, {'x'}, CP_UTF8, L""));
    scheduler.Stop();
    Check(scheduler.Stats().pendingBytes == 0);
    std::cout << "Terminal parse scheduler tests passed\n";
    return 0;
}
