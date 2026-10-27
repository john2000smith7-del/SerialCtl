#include "GuiInstance.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using serialctl::GuiInstance;
void Check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
void Child(const std::filesystem::path &exe, const wchar_t *mode) {
    auto command = L"\"" + exe.wstring() + L"\" " + mode;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    Check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
          "Cannot start instance fixture");
    CloseHandle(process.hThread);
    const auto waited = WaitForSingleObject(process.hProcess, 5000);
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
    Check(waited == WAIT_OBJECT_0 && code == 0, "Second/crashed instance did not finish correctly");
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 2) {
            GuiInstance instance;
            const auto result = instance.Begin();
            if (std::wstring(argv[1]) == L"--second") return result == GuiInstance::Result::Existing ? 0 : 2;
            if (result != GuiInstance::Result::Primary) return 3;
            ExitProcess(0); // Crash-style lifetime: no guard destructor.
        }
        wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
        const auto copyRoot = std::filesystem::temp_directory_path() / (L"serialctl-instance-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(copyRoot);
        const auto copy = copyRoot / L"different-directory.exe";
        std::filesystem::copy_file(path, copy, std::filesystem::copy_options::overwrite_existing);
        {
            GuiInstance first;
            Check(first.Begin() == GuiInstance::Result::Primary, "First GUI did not own the session guard");
            Child(path, L"--second");
            Check(WaitForSingleObject(first.Activation(), 0) == WAIT_OBJECT_0, "Second launch failed to request activation");
            Child(copy, L"--second");
            Check(WaitForSingleObject(first.Activation(), 0) == WAIT_OBJECT_0, "Installation path bypassed single instance");
        }
        Child(path, L"--crash");
        {
            GuiInstance next;
            Check(next.Begin() == GuiInstance::Result::Primary, "Exited/crashed process left a stale instance lock");
        }
        std::filesystem::remove_all(copyRoot);
        std::cout << "Single GUI session, activation, cross-directory launch and crash release PASS\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
