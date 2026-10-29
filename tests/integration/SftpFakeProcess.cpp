// Controlled pipe producer only; never distributed and never contacts a host.
#include <windows.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
int wmain(int argc, wchar_t **argv) {
    bool flood = false, endless = false, probe = false, probeHang = false;
    std::wstring pipe, batchPath;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"-pw") return 40;
        if (arg == L"-pwfile" && i + 1 < argc) pipe = argv[++i];
        if (arg == L"-b" && i + 1 < argc) batchPath = argv[++i];
        flood = flood || arg.find(L"overflow") != std::wstring::npos;
        endless = endless || arg.find(L"cancel") != std::wstring::npos;
        probe = probe || arg == L"exit";
        probeHang = probeHang || arg.find(L"probe-hang") != std::wstring::npos;
    }
    if (pipe.empty()) return 41;
    HANDLE input = CreateFileW(pipe.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE) return 42;
    char password[256]{};
    DWORD n = 0;
    const bool ok = ReadFile(input, password, sizeof(password), &n, nullptr) &&
                    std::string(password, n) == "fixture-password\n";
    SecureZeroMemory(password, sizeof(password));
    CloseHandle(input);
    if (!ok) return 43;
    if (probe) {
        if (probeHang) {
            wchar_t eventName[160]{};
            if (GetEnvironmentVariableW(L"SERIALCTL_TEST_PROBE_READY", eventName, 160)) {
                HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
                if (ready) { SetEvent(ready); CloseHandle(ready); }
            }
            HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            WaitForSingleObject(never, INFINITE);
            CloseHandle(never);
        }
        return 0;
    }
    std::string commands;
    if (!batchPath.empty()) {
        std::ifstream batch{std::filesystem::path(batchPath), std::ios::binary};
        commands.assign(std::istreambuf_iterator<char>(batch), std::istreambuf_iterator<char>());
        if (!batch && !batch.eof()) return 44;
        wchar_t logPath[32768]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_TEST_SFTP_BATCH_LOG", logPath, 32768)) {
            HANDLE log = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log == INVALID_HANDLE_VALUE) return 45;
            const auto record = std::string("BATCH-BEGIN\n") + commands + "BATCH-END\n";
            DWORD written = 0;
            const bool logged = WriteFile(log, record.data(), static_cast<DWORD>(record.size()), &written, nullptr) &&
                                written == record.size();
            CloseHandle(log);
            if (!logged) return 46;
        }
        if (commands.find("serialctl-remove-exact") != std::string::npos) endless = false;
    }
    if (flood || endless) {
        wchar_t eventName[160]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_TEST_OUTPUT_READY", eventName, 160)) {
            HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
            if (ready) { SetEvent(ready); CloseHandle(ready); }
        }
        std::array<char, 4096> bytes{};
        bytes.fill('x');
        for (size_t i = 0; endless || i < 10000; ++i) {
            DWORD written = 0;
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) break;
        }
    }
    return 0;
}
