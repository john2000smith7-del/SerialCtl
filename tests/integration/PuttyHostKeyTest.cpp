#include "PuttyHostKey.h"
#include "Win32Helpers.h"
#include <windows.h>
#include <iostream>
#include <vector>

namespace {
const wchar_t* RegistryPath = L"Software\\SerialCtl\\TrustedHostKeys";
const wchar_t* Fingerprint = L"ssh-ed25519 255 SHA256:test-fingerprint";
std::wstring Executable() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}
void RemoveValue(const std::wstring& host) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RegistryPath, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, (host + L":22").c_str());
        RegCloseKey(key);
    }
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"-batch") {
        bool explicitKey = false, hasKey = false;
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::wstring(argv[i]) == L"-hostkey") {
                hasKey = true;
                explicitKey = std::wstring(argv[i + 1]) == Fingerprint;
            }
        }
        if (explicitKey) return 0;
        if (hasKey) {
            std::cout << "WARNING - HOST KEY DOES NOT MATCH!\nkey fingerprint is:\n"
                         "ssh-ed25519 255 SHA256:changed-key\n";
            return 1;
        }
        std::cout << "The host key is not cached for this server\nkey fingerprint is:\n"
                     "ssh-ed25519 255 SHA256:test-fingerprint\n";
        return 1;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"-verify-persistence") {
        std::wstring key, error;
        return serialctl::ResolvePuttyHostKey(Executable(), argv[2], 22, L"test", L"",
            key, error) && key == Fingerprint ? 0 : 9;
    }
    const std::wstring prefix = L"serialctl-test-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount());
    const std::wstring declined = prefix + L"-decline", accepted = prefix + L"-accept";
    std::wstring key, error;
    int prompts = 0;
    const bool rejected = !serialctl::ResolvePuttyHostKey(Executable(), declined, 22, L"test", L"",
        key, error, nullptr, [&](const std::wstring&, std::uint16_t, const std::wstring&) {
            ++prompts; return false;
        });
    if (!rejected || prompts != 1) { RemoveValue(declined); return 1; }
    key.clear();
    const bool trusted = serialctl::ResolvePuttyHostKey(Executable(), accepted, 22, L"test", L"",
        key, error, nullptr, [&](const std::wstring&, std::uint16_t, const std::wstring& fingerprint) {
            ++prompts; return fingerprint == Fingerprint;
        });
    if (!trusted || prompts != 2 || key != Fingerprint) { RemoveValue(accepted); return 2; }
    const std::wstring changed = prefix + L"-changed";
    HKEY registry = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RegistryPath, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &registry, nullptr) != ERROR_SUCCESS) { RemoveValue(accepted); return 5; }
    const wchar_t oldKey[] = L"ssh-ed25519 255 SHA256:old-key";
    const LONG saved = RegSetValueExW(registry, (changed + L":22").c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(oldKey), sizeof(oldKey));
    RegCloseKey(registry);
    key.clear();
    const bool changedRejected = saved == ERROR_SUCCESS &&
        !serialctl::ResolvePuttyHostKey(Executable(), changed, 22, L"test", L"", key, error,
            nullptr, [&](const std::wstring&, std::uint16_t, const std::wstring&) { ++prompts; return true; });
    RemoveValue(changed);
    if (!changedRejected || prompts != 2) { RemoveValue(accepted); return 6; }
    std::wstring command = serialctl::QuoteCommandLineArgument(Executable()) +
        L" -verify-persistence " + serialctl::QuoteCommandLineArgument(accepted);
    std::vector<wchar_t> buffer(command.begin(), command.end()); buffer.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(Executable().c_str(), buffer.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) { RemoveValue(accepted); return 3; }
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 10);
    DWORD result = 10; GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    RemoveValue(accepted); RemoveValue(declined);
    if (result != 0) return 4;
    std::cout << "Host key confirmation and persistence tests passed\n";
    return 0;
}
