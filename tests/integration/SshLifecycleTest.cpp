#include "PlinkConnection.h"
#include "BackgroundCleanup.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <future>
#include <iostream>
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
int main() {
    try {
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
        nlohmann::json resourceTrace = nlohmann::json::array();
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
            struct StopAndJoin {
                std::shared_ptr<PlinkConnection> ssh; std::thread& sender;
                ~StopAndJoin() { ssh->Stop(); if (sender.joinable()) sender.join(); }
            } stopAndJoin{ssh, sender};
            if (mode == L"noread") Require(writer.wait_for(50ms) == std::future_status::timeout, "nonreading fixture must fill stdin pipe");
            const auto check = GetTickCount64();
            for (int i = 0; i < 1000; ++i) Require(ssh->IsConnected(), "state stays readable during blocked send");
            Require(GetTickCount64() - check < 100, "connection status blocked behind writer");
            const auto start = GetTickCount64();
            BackgroundCleanup cleanup;
            cleanup.Retire([ssh] { ssh->Stop(); });
            Require(GetTickCount64() - start < 100, "UI retirement blocked");
            cleanup.Drain();
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
        SetEnvironmentVariableW(L"SERIALCTL_SSH_READY", nullptr);
        DWORD handlesAfter = 0; GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
        const auto threadsAfter = Threads();
        std::cout << "resource_trace=" << nlohmann::json({{"cycles", resourceTrace}, {"afterCycles", afterCycles}, {"coldHandles", coldHandles}}).dump() << '\n';
        std::cout << "resources handles=" << handlesBefore << "->" << handlesAfter << " threads=" << threadsBefore << "->" << threadsAfter << '\n';
        Require(handlesAfter <= handlesBefore + 3 && threadsAfter <= threadsBefore + 1, "SSH handle/thread growth");
        std::sort(elapsed.begin(), elapsed.end());
        nlohmann::json evidence{{"samples", elapsed}, {"maxMs", elapsed.back()}, {"p50Ms", elapsed[elapsed.size()/2]},
            {"coldHandles", coldHandles}, {"handlesBefore", handlesBefore}, {"handlesAfter", handlesAfter}, {"threadsBefore", threadsBefore}, {"threadsAfter", threadsAfter},
            {"resourceTrace", resourceTrace},
            {"scope", "controlled child processes, no SSH server or UART hardware"}};
        std::cout << evidence.dump() << '\n';
        wchar_t directory[32768]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", directory, 32768)) std::ofstream(std::filesystem::path(directory) / L"ssh-lifecycle.json") << evidence.dump(2);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
