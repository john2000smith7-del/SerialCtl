#include "GuiInstance.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
using serialctl::GuiInstance;
void Check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
HANDLE StartChild(const std::filesystem::path &exe, const wchar_t *mode, const std::wstring &prefix) {
    auto command = L"\"" + exe.wstring() + L"\" " + mode + L" " + prefix;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    Check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
          "Cannot start instance fixture");
    CloseHandle(process.hThread);
    return process.hProcess;
}
void WaitChild(HANDLE process) {
    const auto waited = WaitForSingleObject(process, 5000);
    DWORD code = 1; GetExitCodeProcess(process, &code); CloseHandle(process);
    Check(waited == WAIT_OBJECT_0 && code == 0, "Second/crashed instance did not finish correctly");
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 3) {
            if (std::wstring(argv[1]) == L"--cross") {
                GuiInstance instance(argv[2], L"simulated-other-session");
                const auto result = instance.Begin();
                return result == GuiInstance::Result::CrossSessionExisting ? 0 : 4;
            }
            GuiInstance instance(argv[2]);
            const auto result = instance.Begin();
            if (std::wstring(argv[1]) == L"--second") return result == GuiInstance::Result::Existing ? 0 : 2;
            if (result != GuiInstance::Result::Primary) return 3;
            ExitProcess(0); // Crash-style lifetime: no guard destructor.
        }
        const auto prefix = L"test-" + std::to_wstring(GetCurrentProcessId()) + L"-";
        wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
        const auto copyRoot = std::filesystem::temp_directory_path() / (L"serialctl-instance-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(copyRoot);
        const auto copy = copyRoot / L"different-directory.exe";
        std::filesystem::copy_file(path, copy, std::filesystem::copy_options::overwrite_existing);
        {
            GuiInstance first(prefix);
            Check(first.Begin() == GuiInstance::Result::Primary, "First GUI did not own the session guard");
            WaitChild(StartChild(path, L"--second", prefix));
            Check(WaitForSingleObject(first.Activation(), 0) == WAIT_OBJECT_0, "Second launch failed to request activation");
            const auto owner = CreateWindowExW(0, L"STATIC", L"SerialCtl single-instance fixture",
                                               WS_OVERLAPPEDWINDOW, 0, 0, 420, 250, nullptr, nullptr,
                                               GetModuleHandleW(nullptr), nullptr);
            Check(owner != nullptr, "Create real owner window");
            first.Publish(owner);
            ShowWindow(owner, SW_SHOWNOACTIVATE);
            Check(IsWindowVisible(owner), "Owner window is not visible");
            ShowWindow(owner, SW_MINIMIZE);
            Check(IsIconic(owner), "Owner window did not minimize");
            std::vector<HANDLE> launchers;
            for (int i = 0; i < 10; ++i) launchers.push_back(StartChild(i % 2 ? path : copy, L"--second", prefix));
            for (const auto process : launchers) WaitChild(process);
            Check(WaitForSingleObject(first.Activation(), 0) == WAIT_OBJECT_0,
                  "Parallel launches did not request activation from the one owner");
            first.Activate();
            const auto deadline = GetTickCount64() + 1500;
            while (IsIconic(owner) && GetTickCount64() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
                Sleep(1);
            }
            Check(IsWindowVisible(owner) && !IsIconic(owner), "Activation did not restore the existing owner window");
            WaitChild(StartChild(path, L"--cross", prefix));
            Check(WaitForSingleObject(first.Activation(), 0) == WAIT_TIMEOUT,
                  "Different session must not request activation on the existing desktop");
            DestroyWindow(owner);
        }
        WaitChild(StartChild(path, L"--crash", prefix));
        {
            GuiInstance next(prefix);
            Check(next.Begin() == GuiInstance::Result::Primary, "Exited/crashed process left a stale instance lock");
        }
        std::filesystem::remove_all(copyRoot);
        std::cout << "Single GUI owner, 10 parallel launches from two paths, native restore, simulated cross-session and crash release PASS\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
