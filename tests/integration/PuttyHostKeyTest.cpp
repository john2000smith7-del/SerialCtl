#include "PuttyHostKey.h"
#include "Win32Helpers.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
const wchar_t* RegistryPath = L"Software\\SerialCtl\\TrustedHostKeys";
const wchar_t* FixturePath = L"Software\\SerialCtl\\Tests\\HostKeyRotation";
const wchar_t* Fingerprint = L"ssh-ed25519 255 SHA256:44uodpMmNt68MktdLXKNyvUzZhOewpe0uE2NpYtDoHM";
const wchar_t* Changed = L"ssh-ed25519 255 SHA256:zjS0PTSjix4sGz1bW4OLroa4lt3rkVQawAcHWb+8nd0";
const wchar_t* Third = L"ssh-ed25519 255 SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
std::wstring Executable() {
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH); return path;
}
std::wstring Read(const wchar_t* path, const std::wstring& name) {
    HKEY key = nullptr; wchar_t value[2048]{}; DWORD size = sizeof(value), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    const auto result = RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ ? std::wstring(value) : std::wstring();
}
void Write(const wchar_t* path, const std::wstring& name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot create fixture registry key");
    const auto result = RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                     static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot save fixture registry value");
}
void Remove(const wchar_t* path, const std::wstring& name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name.c_str()); RegCloseKey(key);
    }
}
void Require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
}

int wmain(int argc, wchar_t** argv) {
    using namespace serialctl;
    if (argc > 1 && std::wstring(argv[1]) == L"-batch") {
        const std::wstring host = argc > 2 ? argv[argc - 2] : L"";
        std::wstring pinned, pipe; bool passwordExpected = false;
        for (int i = 1; i + 1 < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"-pw") return 40;
            if (arg == L"-pwfile") pipe = argv[i + 1];
            if (arg == L"-hostkey") pinned = argv[i + 1];
            passwordExpected = passwordExpected || arg == L"password-test";
        }
        if (passwordExpected) {
            HANDLE input = CreateFileW(pipe.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (input == INVALID_HANDLE_VALUE) return 41;
            char password[128]{}; DWORD read = 0;
            const bool ok = ReadFile(input, password, sizeof(password), &read, nullptr) &&
                            std::string(password, read) == "probe-secret\n";
            SecureZeroMemory(password, sizeof(password)); CloseHandle(input);
            if (!ok) return 42;
        }
        if (host.find(L"probe-overflow") != std::wstring::npos) { std::cout << std::string(300 * 1024, 'x'); return 1; }
        const auto mode = Read(FixturePath, host + L"/mode");
        if (mode == L"network") { std::cout << "Network error: Connection refused\n"; return 1; }
        if (mode == L"wildcard") { std::cout << "Host key fingerprint is:\n*\nHost key not in manually configured list\n"; return 1; }
        const auto saved = Read(FixturePath, host);
        const auto current = saved.empty() ? std::wstring(Fingerprint) : saved;
        if (mode != L"no-fingerprint")
            std::cout << "Host key fingerprint is:\n" << WideToMultiByte(current, CP_UTF8) << "\n";
        if (pinned == current || (pinned.empty() && host.find(L"legacy") != std::wstring::npos)) {
            if (mode == L"authentication") { std::cout << "Access denied\n"; return 1; }
            return 0;
        }
        // Match the production PuTTY manual-pin failure, which has no second
        // fingerprint block. Recovery must read the pre-verification -v event.
        if (!pinned.empty()) std::cout << "Host key not in manually configured list\n";
        else std::cout << "The host key is not cached for this server\n";
        return 1;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"-verify-persistence") {
        std::wstring key, error;
        return ResolvePuttyHostKey(Executable(), argv[2], 22, L"test", L"", key, error) && key == Fingerprint ? 0 : 9;
    }
    const auto prefix = L"serialctl-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount());
    const std::vector<std::wstring> hosts{prefix + L"-decline", prefix + L"-accept", prefix + L"-legacy", prefix + L"-race"};
    const auto& declined = hosts[0]; const auto& accepted = hosts[1]; const auto& legacy = hosts[2]; const auto& race = hosts[3];
    auto cleanup = [&] {
        for (const auto& host : hosts) { Remove(RegistryPath, host + L":22"); Remove(RegistryPath, host + L":2222"); Remove(FixturePath, host); Remove(FixturePath, host + L"/mode"); }
    };
    std::wstring key, error;
    auto resolve = [&](const std::wstring& host, HostKeyConfirmation confirm = {}, bool reverify = false) {
        key.clear(); error.clear();
        return ResolvePuttyHostKey(Executable(), host, 22, L"test", L"", key, error, nullptr, confirm, reverify);
    };
    try {
        int prompts = 0;
        Require(!resolve(declined, [&](const HostKeyChallenge& c) {
            ++prompts; Require(c.reason == HostKeyReason::FirstConnection && c.previousFingerprint.empty(), "Wrong first-host challenge"); return false;
        }) && TrustedPuttyHostKey(declined, 22).empty(), "Declining first trust saved a key");
        Require(resolve(accepted, [&](const HostKeyChallenge& c) { ++prompts; return c.fingerprint == Fingerprint; }) &&
                key == Fingerprint && prompts == 2, "First confirmed identity not saved");
        Require(ResolvePuttyHostKey(Executable(), accepted, 22, L"password-test", L"probe-secret", key, error, nullptr, [](const HostKeyChallenge&) { return true; }),
                "Secure password channel regressed");
        Require(!resolve(prefix + L"-probe-overflow") && error.find(L"256 KiB") != std::wstring::npos, "Probe output limit missing");
        const auto command = QuoteCommandLineArgument(Executable()) + L" -verify-persistence " + QuoteCommandLineArgument(accepted);
        std::vector<wchar_t> buffer(command.begin(), command.end()); buffer.push_back(0);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        Require(CreateProcessW(Executable().c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &process) != FALSE, "Cannot start persistence probe");
        if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 10);
        DWORD result = 10; GetExitCodeProcess(process.hProcess, &result);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        Require(result == 0, "Approved identity did not survive a fresh process");

        Write(FixturePath, accepted, Changed);
        Write(RegistryPath, declined + L":22", Fingerprint); Write(FixturePath, declined, Changed);
        Require(!resolve(declined), "Changed persisted host accepted without a confirmation callback");
        Require(!resolve(accepted, [&](const HostKeyChallenge& c) {
            Require(c.reason == HostKeyReason::Changed && c.previousFingerprint == Fingerprint && c.fingerprint == Changed,
                    "Changed-host dialog lost the old/new identity"); return false;
        }) && TrustedPuttyHostKey(accepted, 22) == Fingerprint, "Cancel replaced old identity");
        Write(RegistryPath, accepted + L":2222", Third);
        Require(resolve(accepted, [](const HostKeyChallenge&) { return true; }) && key == Changed &&
                TrustedPuttyHostKey(accepted, 22) == Changed && TrustedPuttyHostKey(accepted, 2222) == Third,
                "Confirmed rotation did not replace only the selected endpoint");
        Require(resolve(accepted) && key == Changed, "Shared SFTP cache retained the old pin after replacement");
        Require(resolve(accepted, [&](const HostKeyChallenge& c) {
            Require(c.reason == HostKeyReason::Reverify && c.previousFingerprint == Changed && c.fingerprint == Changed,
                    "Manual recheck skipped a cached unchanged host"); return true;
        }, true), "Manual recheck failed");
        Write(FixturePath, accepted + L"/mode", L"network");
        Require(!resolve(accepted, [](const HostKeyChallenge&) -> bool { throw std::runtime_error("Network error asked to trust a key"); }) &&
                TrustedPuttyHostKey(accepted, 22) == Changed, "Network failure erased the identity");
        Remove(FixturePath, accepted + L"/mode");

        Write(FixturePath, accepted, Fingerprint);
        Require(!resolve(accepted, [&](const HostKeyChallenge&) { Write(FixturePath, accepted, Third); return true; }) &&
                TrustedPuttyHostKey(accepted, 22) == Changed, "A second rotation replaced the approved pin");
        Write(FixturePath, accepted, Fingerprint);
        std::atomic_bool cancel{false};
        Require(!ResolvePuttyHostKey(Executable(), accepted, 22, L"test", L"", key, error, &cancel,
                [&](const HostKeyChallenge&) { cancel = true; return true; }) && TrustedPuttyHostKey(accepted, 22) == Changed,
                "Cancellation committed a replacement");
        Write(FixturePath, accepted + L"/mode", L"no-fingerprint");
        Require(!resolve(accepted, [](const HostKeyChallenge&) -> bool { throw std::runtime_error("Missing fingerprint shown for trust"); }) &&
                TrustedPuttyHostKey(accepted, 22) == Changed, "Missing fingerprint changed the identity");
        Remove(FixturePath, accepted + L"/mode");
        Require(!resolve(accepted, [&](const HostKeyChallenge&) { Write(FixturePath, accepted + L"/mode", L"network"); return true; }) &&
                TrustedPuttyHostKey(accepted, 22) == Changed, "A failed retry erased the previous pin");
        Remove(FixturePath, accepted + L"/mode");
        Write(FixturePath, declined + L"/mode", L"wildcard");
        Require(!resolve(declined, [](const HostKeyChallenge&) -> bool { throw std::runtime_error("Wildcard offered as a fingerprint"); }) &&
                TrustedPuttyHostKey(declined, 22) == Fingerprint, "A wildcard bypassed strict fingerprint validation");
        Remove(FixturePath, declined + L"/mode");
        Write(RegistryPath, declined + L":22", L"*");
        Require(!resolve(declined), "Malformed saved pin was supplied as a permissive PuTTY option");
        Require(resolve(declined, [](const HostKeyChallenge&) { return true; }, true) && key == Changed,
                "Manual verification could not recover a malformed identity record");
        Write(FixturePath, accepted + L"/mode", L"authentication");
        Require(!resolve(accepted, [](const HostKeyChallenge&) { return true; }) &&
                TrustedPuttyHostKey(accepted, 22) == Fingerprint && IsPuttyAuthenticationFailure(error),
                "Password rejection erased a separately verified/approved identity");
        Remove(FixturePath, accepted + L"/mode");

        Require(resolve(legacy) && key == Fingerprint && TrustedPuttyHostKey(legacy, 22) == Fingerprint,
                "Verified legacy PuTTY identity was not migrated to an explicit pin");
        Write(RegistryPath, race + L":22", Fingerprint); Write(FixturePath, race, Changed);
        Require(!resolve(race, [&](const HostKeyChallenge&) { Write(RegistryPath, race + L":22", Third); return true; }) &&
                TrustedPuttyHostKey(race, 22) == Third, "A stale approval overwrote a newer identity record");
        Require(!IsPuttyHostKeyFailure(L"Access denied") && !IsPuttyHostKeyFailure(L"Connection timed out") &&
                IsPuttyHostKeyFailure(L"Host key not in manually configured list"), "Host-key errors misclassified");
        cleanup();
        std::cout << "Host identity rotation, cancellation, persistence, shared cache and race tests passed\n";
        return 0;
    } catch (const std::exception& failure) {
        cleanup(); std::cerr << failure.what() << '\n'; return 1;
    }
}
