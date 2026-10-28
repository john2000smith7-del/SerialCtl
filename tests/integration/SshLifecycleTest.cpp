#include "PlinkConnection.h"
#include "BackgroundCleanup.h"
#include "json.hpp"
#include <algorithm>
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
        DWORD handlesBefore = 0; GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
        const auto threadsBefore = Threads();
        std::vector<ULONGLONG> elapsed;
        std::wstring error;
        for (int index = 0; index < 16; ++index) {
            const std::wstring mode = index < 8 ? L"noread" : index < 12 ? L"flood" : L"echo";
            const auto host = L"serialctl-lifecycle-" + std::to_wstring(GetCurrentProcessId()) + mode;
            const auto eventName = L"Local\\SerialCtl.SSH." + std::to_wstring(GetCurrentProcessId());
            HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
            SetEnvironmentVariableW(L"SERIALCTL_SSH_READY", eventName.c_str());
            auto ssh = std::make_shared<PlinkConnection>(host, 22, L"fixture", L"", 80, 24, [](const auto&) { return true; });
            std::atomic<size_t> received{0};
            Require(ssh->Start([&](const Bytes& bytes) { received += bytes.size(); }, [](const auto&, bool) {}, error), "SSH fixture start");
            Require(WaitForSingleObject(ready, 3000) == WAIT_OBJECT_0, "child ready");
            CloseHandle(ready);
            auto writer = std::async(std::launch::async, [ssh] { std::wstring message; return ssh->Send(Bytes(8 * 1024 * 1024, 'x'), message); });
            if (mode == L"noread") Require(writer.wait_for(50ms) == std::future_status::timeout, "nonreading fixture must fill stdin pipe");
            const auto check = GetTickCount64();
            for (int i = 0; i < 1000; ++i) Require(ssh->IsConnected(), "state stays readable during blocked send");
            Require(GetTickCount64() - check < 100, "connection status blocked behind writer");
            const auto start = GetTickCount64();
            BackgroundCleanup cleanup;
            cleanup.Retire([ssh] { ssh->Stop(); });
            Require(GetTickCount64() - start < 100, "UI retirement blocked");
            cleanup.Drain();
            elapsed.push_back(GetTickCount64() - start);
            Require(elapsed.back() < 1500, "stop deadline");
            Require(writer.wait_for(0ms) == std::future_status::ready, "writer survived Stop");
            writer.get(); ssh->Stop();
            Require(!ssh->IsConnected(), "repeat Stop state");
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SerialCtl\\TrustedHostKeys", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, (host + L":22").c_str()); RegCloseKey(key);
            }
        }
        {
            const auto eventName = L"Local\\SerialCtl.SSH.Cancel." + std::to_wstring(GetCurrentProcessId());
            HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
            SetEnvironmentVariableW(L"SERIALCTL_SSH_READY", eventName.c_str());
            PlinkConnection ssh(L"slowprobe", 22, L"fixture", L"", 80, 24);
            auto start = std::async(std::launch::async, [&] { return ssh.Start({}, {}, error); });
            Require(WaitForSingleObject(ready, 3000) == WAIT_OBJECT_0, "probe ready before cancellation");
            ssh.CancelStart(); Require(start.wait_for(1500ms) == std::future_status::ready && !start.get(), "startup cancellation");
            ssh.Stop(); CloseHandle(ready);
        }
        SetEnvironmentVariableW(L"SERIALCTL_SSH_READY", nullptr);
        DWORD handlesAfter = 0; GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
        Require(handlesAfter <= handlesBefore + 3 && Threads() <= threadsBefore + 1, "SSH handle/thread growth");
        std::sort(elapsed.begin(), elapsed.end());
        nlohmann::json evidence{{"samples", elapsed}, {"maxMs", elapsed.back()}, {"p50Ms", elapsed[elapsed.size()/2]},
            {"handlesBefore", handlesBefore}, {"handlesAfter", handlesAfter}, {"scope", "controlled child processes, no SSH server or UART hardware"}};
        std::cout << evidence.dump() << '\n';
        wchar_t directory[32768]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", directory, 32768)) std::ofstream(std::filesystem::path(directory) / L"ssh-lifecycle.json") << evidence.dump(2);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
