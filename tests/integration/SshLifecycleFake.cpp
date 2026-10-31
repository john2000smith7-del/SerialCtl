// Offline PuTTY stand-in; never packaged or used to contact a server.
#include <windows.h>
#include <array>
#include <iostream>
#include <string>
int wmain(int argc, wchar_t** argv) {
    bool probe = false;
    for (int i = 1; i < argc; ++i) probe = probe || std::wstring(argv[i]) == L"exit";
    const std::wstring host = argc > 1 ? argv[argc - (probe ? 2 : 1)] : L"";
    if (probe) {
        if (host.find(L"slowprobe") != std::wstring::npos) {
            wchar_t name[256]{}; GetEnvironmentVariableW(L"SERIALCTL_SSH_READY", name, 256);
            HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
            if (event) { SetEvent(event); CloseHandle(event); }
            HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            WaitForSingleObject(never, INFINITE); CloseHandle(never);
        }
        std::cout << "Host key fingerprint is:\nssh-ed25519 255 SHA256:44uodpMmNt68MktdLXKNyvUzZhOewpe0uE2NpYtDoHM\n" << std::flush;
        return 0;
    }
    wchar_t name[256]{}; GetEnvironmentVariableW(L"SERIALCTL_SSH_READY", name, 256);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (ready) { SetEvent(ready); CloseHandle(ready); }
    if (host.find(L"authfail") != std::wstring::npos) { std::cerr << "FATAL ERROR: Access denied\n" << std::flush; return 1; }
    std::cerr << "Started a shell/command\n" << std::flush;
    if (host.find(L"crash") != std::wstring::npos) { Sleep(200); return 7; }
    if (host.find(L"noread") != std::wstring::npos) {
        HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        WaitForSingleObject(never, INFINITE); CloseHandle(never);
    }
    std::array<char, 4096> bytes{};
    for (;;) {
        DWORD n = 0, written = 0;
        if (host.find(L"flood") != std::wstring::npos) n = static_cast<DWORD>(bytes.size());
        else if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) || !n) break;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), n, &written, nullptr)) break;
    }
    return 0;
}
