#include "PlinkConnection.h"
#include "BackgroundCleanup.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <tlhelp32.h>
using namespace serialctl;
using namespace std::chrono_literals;
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
DWORD Threads() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{sizeof(entry)}; DWORD count = 0;
    if (Thread32First(snapshot, &entry)) do { if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count; } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot); return count;
}
int main(int argc, char** argv) {
    try {
        const bool delayResizeStart = argc == 2 && std::string(argv[1]) == "--delay-resize-start";
        Require(argc == 1 || delayResizeStart, "expected optional --delay-resize-start");
        // Warm GUID/pipe runtime state. The harness uses joined std::threads,
        // not MSVC's reusable async pool, so its cached workers cannot be
        // mistaken for leaked transport threads.
        HANDLE warmParent = nullptr, warmChild = nullptr;
        Require(CreateChildIoPipe(warmParent, warmChild, true), "warm pipe runtime");
        CloseHandle(warmParent); CloseHandle(warmChild);
        DWORD handlesBefore = 0; GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
        const DWORD coldHandles = handlesBefore;
        auto threadsBefore = Threads();
        std::vector<ULONGLONG> elapsed;
        size_t concurrentResizeCalls = 0;
        nlohmann::json resourceTrace = nlohmann::json::array();
        nlohmann::json resizeStartup = nlohmann::json::array();
        std::wstring error;
        // First complete cycle initializes process/CRT synchronization caches.
        // The diagnostic trace established 123 -> 128 once, then exactly 128
        // handles and four threads throughout all 16 cycles and edge cases.
        for (int index = -1; index < 16; ++index) {
          {
            const std::wstring mode = index < 8 ? L"noread" : index < 12 ? L"flood" : L"echo";
            const auto host = L"serialctl-lifecycle-" + std::to_wstring(GetCurrentProcessId()) + mode;
            const auto eventName = L"Local\\SerialCtl.SSH." + std::to_wstring(GetCurrentProcessId());
            HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
            SetEnvironmentVariableW(L"SERIALCTL_SSH_READY", eventName.c_str());
            HostKeyConfirmation confirm = [](const HostKeyChallenge&) { return true; };
            auto ssh = std::make_shared<PlinkConnection>(host, std::uint16_t(22), L"fixture", L"", 80, 24, confirm);
            std::atomic<size_t> received{0};
            Require(ssh->Start([&](const Bytes& bytes) { received += bytes.size(); }, [](const auto&, bool) {}, error), "SSH fixture start");
            Require(WaitForSingleObject(ready, 3000) == WAIT_OBJECT_0, "child ready");
            CloseHandle(ready);
            std::packaged_task<bool()> writeTask([ssh] { std::wstring message; return ssh->Send(Bytes(8 * 1024 * 1024, 'x'), message); });
            auto writer = writeTask.get_future();
            std::thread sender(std::move(writeTask));
            std::atomic_bool resizing{false};
            std::atomic_size_t resizeCalls{0};
            std::thread resizer;
            // Keep the promise alive until StopAndJoin has joined the worker,
            // including when readiness times out and an assertion unwinds.
            std::promise<void> resizeStarted;
            auto resizeReady = resizeStarted.get_future();
            struct StopAndJoin {
                std::shared_ptr<PlinkConnection> ssh; std::thread& sender;
                std::atomic_bool& resizing; std::thread& resizer;
                ~StopAndJoin() {
                    resizing = false;
                    if (resizer.joinable()) resizer.join();
                    ssh->Stop(); if (sender.joinable()) sender.join();
                }
            } stopAndJoin{ssh, sender, resizing, resizer};
            if (mode == L"noread") Require(writer.wait_for(50ms) == std::future_status::timeout, "nonreading fixture must fill stdin pipe");
            const auto check = GetTickCount64();
            for (int i = 0; i < 1000; ++i) Require(ssh->IsConnected(), "state stays readable during blocked send");
            Require(GetTickCount64() - check < 100, "connection status blocked behind writer");
            // The production queue now delivers dimensions on its writer.
            // Keep issuing real mapped-view updates through Stop's channel
            // teardown, including calls after Stop, not just before closing.
            resizing = true;
            const auto resizeStart = GetTickCount64();
            resizer = std::thread([&] {
                // A controlled scheduler delay is test setup, not a resize or
                // teardown operation. Only the warm-up cycle injects it.
                if (delayResizeStart && index == -1) Sleep(600);
                resizeStarted.set_value();
                while (resizing) {
                    const auto count = resizeCalls.load();
                    ssh->ResizeTerminal(80 + int(count % 100), 24 + int(count % 40));
                    ++resizeCalls;
                    std::this_thread::yield();
                }
            });
            Require(resizeReady.wait_for(3000ms) == std::future_status::ready, "resize worker startup");
            resizeReady.get();
            const auto startupMs = GetTickCount64() - resizeStart;
            const auto resizeDeadline = GetTickCount64() + 500;
            while (resizeCalls < 100 && GetTickCount64() < resizeDeadline) Sleep(1);
            // Capture at the deadline: diagnostics must not let a late worker
            // satisfy the original assertion while console output is written.
            const auto completedResizes = resizeCalls.load();
            resizeStartup.push_back({{"cycle", index}, {"startupMs", startupMs}, {"callsBeforeStop", completedResizes}});
            std::cout << "resize_startup=" << resizeStartup.back().dump() << std::endl;
            Require(completedResizes >= 100, "concurrent resize writer did not run");
            const auto start = GetTickCount64();
            BackgroundCleanup cleanup;
            cleanup.Retire([ssh] { ssh->Stop(); });
            Require(GetTickCount64() - start < 100, "UI retirement blocked");
            cleanup.Drain();
            const auto afterStop = resizeCalls.load();
            const auto postStopDeadline = GetTickCount64() + 500;
            while (resizeCalls < afterStop + 100 && GetTickCount64() < postStopDeadline) Sleep(1);
            Require(resizeCalls >= afterStop + 100, "resize after channel teardown did not finish");
            resizing = false; resizer.join();
            concurrentResizeCalls += resizeCalls.load();
            const auto stoppedAfter = GetTickCount64() - start;
            Require(stoppedAfter < 1500, "stop deadline");
            // Stop drains the transport write under its mutex. The caller still
            // needs to publish its promise AFTER returning from Send; that is a
            // separate scheduling step. Keep the same 1500 ms total deadline
            // rather than incorrectly requiring a zero-time future transition.
            Require(writer.wait_for(std::chrono::milliseconds(1500 - stoppedAfter)) == std::future_status::ready,
                    "writer did not finish within the stop deadline");
            writer.get(); sender.join(); ssh->Stop();
            elapsed.push_back(GetTickCount64() - start);
            Require(elapsed.back() < 1500, "stop and sender completion deadline");
            Require(!ssh->IsConnected(), "repeat Stop state");
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SerialCtl\\TrustedHostKeys", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, (host + L":22").c_str()); RegCloseKey(key);
            }
          }
          DWORD count = 0; GetProcessHandleCount(GetCurrentProcess(), &count);
          resourceTrace.push_back({{"cycle", index}, {"handles", count}, {"threads", Threads()}});
          if (index < 0) { handlesBefore = count; threadsBefore = Threads(); elapsed.clear(); }
          else Require(count <= handlesBefore + 3 && Threads() <= threadsBefore + 1, "per-cycle SSH resource growth");
        }
        DWORD afterCycles = 0; GetProcessHandleCount(GetCurrentProcess(), &afterCycles);
        {
            const auto eventName = L"Local\\SerialCtl.SSH.Cancel." + std::to_wstring(GetCurrentProcessId());
            HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
            SetEnvironmentVariableW(L"SERIALCTL_SSH_READY", eventName.c_str());
            PlinkConnection ssh(L"slowprobe", 22, L"fixture", L"", 80, 24);
            std::packaged_task<bool()> startTask([&] { return ssh.Start({}, {}, error); });
            auto start = startTask.get_future();
            std::thread starter(std::move(startTask));
            struct CancelAndJoin {
                PlinkConnection& ssh; std::thread& starter;
                ~CancelAndJoin() { ssh.CancelStart(); if (starter.joinable()) starter.join(); ssh.Stop(); }
            } cancelAndJoin{ssh, starter};
            Require(WaitForSingleObject(ready, 3000) == WAIT_OBJECT_0, "probe ready before cancellation");
            ssh.CancelStart(); Require(start.wait_for(1500ms) == std::future_status::ready && !start.get(), "startup cancellation");
            starter.join(); ssh.Stop(); CloseHandle(ready);
        }
        {
            PlinkConnection crash(L"serialctl-lifecycle-crash", 22, L"fixture", L"", 80, 24);
            HANDLE ended = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Require(crash.Start({}, [&](const std::wstring&, bool failure) { if (failure) SetEvent(ended); }, error), "crash fixture start");
            Require(WaitForSingleObject(ended, 3000) == WAIT_OBJECT_0 && !crash.IsConnected(), "sudden child exit updates atomic connection state");
            crash.Stop(); crash.Stop(); CloseHandle(ended);
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SerialCtl\\TrustedHostKeys", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, L"serialctl-lifecycle-crash:22"); RegCloseKey(key);
            }
        }
        {
            PlinkConnection denied(L"serialctl-lifecycle-authfail",22,L"fixture",L"",80,24);
            Require(!denied.Start({}, {}, error) && !denied.IsConnected(), "post-probe authentication failure must not become a successful session/history");
        }
        SetEnvironmentVariableW(L"SERIALCTL_SSH_READY", nullptr);
        DWORD handlesAfter = 0; GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
        const auto threadsAfter = Threads();
        std::cout << "resource_trace=" << nlohmann::json({{"cycles", resourceTrace}, {"afterCycles", afterCycles}, {"coldHandles", coldHandles}}).dump() << '\n';
        std::cout << "resources handles=" << handlesBefore << "->" << handlesAfter << " threads=" << threadsBefore << "->" << threadsAfter << '\n';
        Require(handlesAfter <= handlesBefore + 3 && threadsAfter <= threadsBefore + 1, "SSH handle/thread growth");
        std::sort(elapsed.begin(), elapsed.end());
        nlohmann::json evidence{{"samples", elapsed}, {"maxMs", elapsed.back()}, {"p50Ms", elapsed[elapsed.size()/2]},
            {"concurrentResizeCalls", concurrentResizeCalls}, {"resizeThroughStop", "PASS"},
            {"coldHandles", coldHandles}, {"handlesBefore", handlesBefore}, {"handlesAfter", handlesAfter}, {"threadsBefore", threadsBefore}, {"threadsAfter", threadsAfter},
            {"resourceTrace", resourceTrace},
            {"resizeStartup", resizeStartup}, {"startupDelayInjected", delayResizeStart},
            {"scope", "controlled child processes, no SSH server or UART hardware"}};
        std::cout << evidence.dump() << '\n';
        wchar_t directory[32768]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", directory, 32768)) {
            const auto file = delayResizeStart ? L"ssh-lifecycle-delayed-start.json" : L"ssh-lifecycle.json";
            std::ofstream(std::filesystem::path(directory) / file) << evidence.dump(2);
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
