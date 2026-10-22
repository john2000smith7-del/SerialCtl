#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winioctl.h>
#include "UpdateInstaller.h"
#include "UpdateZipFixture.h"
#include "UpdateManifest.h"
#include "Win32Helpers.h"
#include "json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using namespace serialctl::update;
using Bytes = std::vector<unsigned char>;
using Json = nlohmann::json;
namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    std::cout << "[PASS] " << message << std::endl;
}
void Reject(const std::function<void()>& action, const char* message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    Check(failed, message);
}
fs::path Self() {
    wchar_t value[32768]{};
    DWORD count = GetModuleFileNameW(nullptr, value, 32768);
    if (!count || count >= 32768) throw std::runtime_error("GetModuleFileName failed");
    return fs::path(std::wstring(value, count));
}
void Write(const fs::path& path, const std::string& value) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!file) throw std::runtime_error("Fixture write failed");
}
void Write(const fs::path& path, const Bytes& value) {
    Write(path, std::string(value.begin(), value.end()));
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
bool WaitFile(const fs::path& path, DWORD timeout = 15000) {
    const ULONGLONG started = GetTickCount64();
    do { if (fs::exists(path)) return true; Sleep(25); } while (GetTickCount64() - started < timeout);
    return false;
}
bool WaitText(const fs::path& path, const std::string& text, DWORD timeout = 15000) {
    const ULONGLONG started = GetTickCount64();
    do { if (Read(path).find(text) != std::string::npos) return true; Sleep(25); } while (GetTickCount64() - started < timeout);
    return false;
}
struct Process {
    HANDLE process = nullptr;
    ~Process() { if (process) { if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) { TerminateProcess(process, 99); WaitForSingleObject(process, 5000); } CloseHandle(process); } }
    Process(const fs::path& executable, const std::wstring& parameters) {
        std::wstring command = serialctl::QuoteCommandLineArgument(executable.wstring()) + L" " + parameters;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, executable.parent_path().c_str(), &startup, &info))
            throw std::runtime_error("Fixture process launch failed: " + std::to_string(GetLastError()));
        process = info.hProcess; CloseHandle(info.hThread);
    }
    DWORD Wait(DWORD timeout = 20000) {
        if (WaitForSingleObject(process, timeout) != WAIT_OBJECT_0) throw std::runtime_error("Fixture process did not exit");
        DWORD code = 0; GetExitCodeProcess(process, &code); return code;
    }
};
struct Junction {
    fs::path path;
    Junction(const fs::path& link, const fs::path& target) : path(link) {
        // NTFS directory junctions require no symbolic-link privilege. This
        // exercises the actual filesystem alias, not a mocked pathname parser.
        fs::create_directory(path);
        struct Data {
            DWORD tag; WORD length, reserved;
            WORD substituteOffset, substituteLength, printOffset, printLength;
            wchar_t paths[1];
        };
        const std::wstring substitute = L"\\??\\" + fs::absolute(target).wstring();
        const std::wstring printable = fs::absolute(target).wstring();
        const auto pathBytes = (substitute.size() + printable.size() + 2) * sizeof(wchar_t);
        std::vector<unsigned char> bytes(offsetof(Data, paths) + pathBytes, 0);
        auto* data = reinterpret_cast<Data*>(bytes.data());
        data->tag = IO_REPARSE_TAG_MOUNT_POINT;
        data->length = static_cast<WORD>(8 + pathBytes);
        data->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
        data->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
        data->printLength = static_cast<WORD>(printable.size() * sizeof(wchar_t));
        std::memcpy(data->paths, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
        std::memcpy(reinterpret_cast<unsigned char*>(data->paths) + data->printOffset,
                    printable.c_str(), (printable.size() + 1) * sizeof(wchar_t));
        HANDLE directory = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (directory == INVALID_HANDLE_VALUE) { RemoveDirectoryW(path.c_str()); throw std::runtime_error("Cannot open junction fixture"); }
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, bytes.data(),
            static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr);
        CloseHandle(directory);
        if (!ok) { RemoveDirectoryW(path.c_str()); throw std::runtime_error("Cannot create NTFS junction fixture"); }
    }
    ~Junction() { RemoveDirectoryW(path.c_str()); }
};
struct Key {
    HCRYPTPROV provider = 0;
    HCRYPTKEY key = 0;
    Bytes publicKey;
    Key() {
        if (!CryptAcquireContextW(&provider, nullptr, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
            !CryptGenKey(provider, AT_SIGNATURE, (2048u << 16) | CRYPT_EXPORTABLE, &key)) throw std::runtime_error("Test key generation failed");
        DWORD size = 0;
        if (!CryptExportKey(key, 0, PUBLICKEYBLOB, 0, nullptr, &size)) throw std::runtime_error("Test public key export failed");
        publicKey.resize(size);
        if (!CryptExportKey(key, 0, PUBLICKEYBLOB, 0, publicKey.data(), &size)) throw std::runtime_error("Test public key export failed");
    }
    ~Key() { if (key) CryptDestroyKey(key); if (provider) CryptReleaseContext(provider, 0); }
    Bytes Sign(const std::string& text) {
        HCRYPTHASH hash = 0; DWORD size = 256; Bytes signature(size);
        if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) throw std::runtime_error("Signature hash failed");
        const bool ok = CryptHashData(hash, reinterpret_cast<const BYTE*>(text.data()), static_cast<DWORD>(text.size()), 0) &&
            CryptSignHashW(hash, AT_SIGNATURE, nullptr, 0, signature.data(), &size);
        CryptDestroyHash(hash);
        if (!ok || size != 256) throw std::runtime_error("Fixture signing failed");
        std::reverse(signature.begin(), signature.end()); return signature;
    }
};
struct Fixture {
    fs::path root, stage;
    explicit Fixture(unsigned index) {
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temporary)) throw std::runtime_error("Temp path failed");
        root = fs::path(temporary) / (L"serialctl-installer-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(index));
        std::ostringstream token; token << std::hex << std::setw(32) << std::setfill('0') << index;
        stage = root / L".serialctl-update" / token.str();
        fs::create_directories(stage / L"files");
        Write(root / L"serialctl.exe", "old-exe"); Write(root / L"plink.exe", "old-plink"); Write(root / L"psftp.exe", "old-psftp");
        Write(root / L"VERSION", "V1.0.0\n"); Write(root / L"update.ini", "private user update URL");
        fs::create_directory(root / L"drivers"); Write(root / L"drivers" / L"driver.txt", "preserve driver");
        Write(stage / L"files" / L"serialctl.exe", "new-exe"); Write(stage / L"files" / L"plink.exe", "new-plink");
        Write(stage / L"files" / L"psftp.exe", "new-psftp"); Write(stage / L"files" / L"VERSION", "V9.0.0\n");
        Write(stage / L"files" / L"README.md", "new-readme");
    }
    ~Fixture() {
        // Children signal only after their production handshake. A brief bounded
        // retry also waits for Windows to release the mapped test executable.
        for (int retry = 0; retry < 100; ++retry) {
            std::error_code ec; fs::remove_all(root, ec); if (!ec) break; Sleep(25);
        }
    }
    fs::path Journal() const { return root / L".serialctl-update" / L"transaction.txt"; }
    void OriginalRestored() {
        Check(Read(root / L"serialctl.exe") == "old-exe" && Read(root / L"plink.exe") == "old-plink" &&
            Read(root / L"psftp.exe") == "old-psftp" && Read(root / L"VERSION") == "V1.0.0\n", "all original bytes restored");
        Check(!fs::exists(root / L"README.md"), "newly added file removed by rollback");
        Check(Read(root / L"update.ini") == "private user update URL" && Read(root / L"drivers" / L"driver.txt") == "preserve driver", "configuration and driver bytes preserved");
    }
    void ExecutablePayload(Key &key, bool embedded = false) {
        fs::copy_file(Self(), root / L"serialctl.exe", fs::copy_options::overwrite_existing);
        fs::copy_file(Self(), stage / L"files" / L"serialctl.exe", fs::copy_options::overwrite_existing);
        Write(stage / L"files" / L"VERSION", "PROJECT_VERSION=V9.0.0\nCPP_VERSION=V9.0.0\n");
        Json manifest{{"schema", 1},
                      {"product", "SerialCtl"},
                      {"architecture", "x64"},
                      {"version", "V9.0.0"},
                      {"files", Json::array()}};
        for (const auto &item : fs::directory_iterator(stage / L"files")) {
            const auto name = item.path().filename().u8string();
            manifest["files"].push_back({{"name", name},
                                         {"size", fs::file_size(item.path())},
                                         {"sha256", Sha256File(item.path())},
                                         {"url", "files/" + name}});
        }
        if (embedded) {
            std::map<std::string, testing::ZipBytes> members;
            for (const auto &item : fs::directory_iterator(stage / L"files")) {
                const auto bytes = Read(item.path());
                members[item.path().filename().u8string()] = testing::ZipBytes(bytes.begin(), bytes.end());
            }
            const auto archive = testing::BuildTestZip(members, true);
            Write(stage / L"package.zip", archive);
            manifest["schema"] = 2;
            manifest["archive"] = {
                {"url", "test-update.zip"}, {"size", archive.size()}, {"sha256", Sha256File(stage / L"package.zip")}};
            const auto raw = manifest.dump();
            std::string signature;
            constexpr char digits[] = "0123456789abcdef";
            for (auto byte : key.Sign(raw)) {
                signature += digits[byte >> 4];
                signature += digits[byte & 15];
            }
            Write(stage / L"update.json", Json{{"schema", 2}, {"payload", raw}, {"signature", signature}}.dump());
        } else {
            const auto raw = manifest.dump();
            Write(stage / L"update.json", raw);
            Write(stage / L"update.json.sig", key.Sign(raw));
        }
        Write(stage / L"test-public-key.bin", key.publicKey);
    }
};
std::wstring Parameters(int count, wchar_t** args) {
    std::wstring result;
    for (int i = 1; i < count; ++i) { if (i > 1) result += L" "; result += serialctl::QuoteCommandLineArgument(args[i]); }
    return result;
}
// Child modes are present exclusively in this non-shipped integration binary.
int Child(int count, wchar_t** args, bool& handled) {
    handled = count > 1;
    if (!handled) return 0;
    const std::wstring mode = args[1];
    std::wstring parameters = Parameters(count, args);
    // The production parser expects the flag at the first character.
    if (parameters.front() == L'"') {
        const auto end = parameters.find(L'"', 1);
        parameters = parameters.substr(1, end - 1) + parameters.substr(end + 1);
    }
    int result = 0;
    if (HandleUpdateCommandLine(parameters, result)) return result;
    const fs::path root = Self().parent_path();
    StartupGuard guard; std::wstring error;
    const bool normal = mode == L"--test-normal-start" || mode == L"--test-update-parent" || mode == L"--test-refused-update-parent" || mode == L"--test-hold" || mode == L"--test-junction-hold";
    auto action = guard.Begin(normal ? L"" : parameters, error);
    if (action == StartupAction::Exit) return 0;
    if (action != StartupAction::Continue) { Write(root / L"child-error.txt", serialctl::WideToMultiByte(error, CP_UTF8)); return 31; }
    std::string runningVersion = "V1.0.0";
    if (mode == L"--serialctl-update-health") {
        const auto behavior = Read(root / L"health-mode.txt");
        if (behavior == "exit") return 32;
        if (behavior == "hang") { Sleep(10000); return 33; }
        if (behavior != "version-mismatch") runningVersion = "V9.0.0";
    }
    HWND window = CreateWindowExW(0, L"STATIC", L"SerialCtl updater integration health window", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 34;
    if (!guard.Confirm(error, runningVersion)) { Write(root / L"child-error.txt", serialctl::WideToMultiByte(error, CP_UTF8)); DestroyWindow(window); return 35; }
    DestroyWindow(window);
    if (mode == L"--test-update-parent") {
        if (count != 3 || !testing::LaunchSignedTestUpdate(fs::path(args[2]), "V1.0.0", error)) {
            Write(root / L"launch-error.txt", serialctl::WideToMultiByte(error, CP_UTF8)); return 36;
        }
        return 0; // real normal parent exit, never a forced termination
    }
    if (mode == L"--test-refused-update-parent") {
        if (count != 3) return 39;
        if (testing::LaunchSignedTestUpdateWithoutApproval(fs::path(args[2]), "V1.0.0", error)) return 40;
        Write(root / L"launch-error.txt", serialctl::WideToMultiByte(error, CP_UTF8));
        return 36; // The still-alive helper must not treat this exit as approval.
    }
    if (mode == L"--test-hold" || mode == L"--test-junction-hold") {
        if (mode == L"--test-junction-hold" && guard.ShouldCheckForUpdate()) return 38;
        Write(root / L"holder-ready.txt", "ready"); Sleep(10000); return 0;
    }
    if (mode == L"--serialctl-update-recovered") {
        if (guard.ShouldCheckForUpdate()) return 37;
        Write(root / L"recovered.txt", "restarted with update check suppressed");
    } else Write(root / L"healthy.txt", "main window created and transaction committed");
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        bool handled = false;
        const int childResult = Child(argc, argv, handled);
        if (handled) return childResult;
        {
            Fixture f(1);
            testing::ApplyTransaction(f.root, f.stage);
            Check(Read(f.root / L"serialctl.exe") == "new-exe" && Read(f.root / L"plink.exe") == "new-plink" &&
                Read(f.root / L"psftp.exe") == "new-psftp", "real Win32 files replaced before health confirmation");
            testing::RecoverTransaction(f.root); f.OriginalRestored();
            Check(!fs::exists(f.Journal()), "rollback removes completed journal");
        }
        {
            Fixture f(2);
            Reject([&] { testing::ApplyTransaction(f.root, f.stage, 2); }, "replacement fault injected after two files");
            Check(fs::exists(f.Journal()), "partial transaction has durable recovery journal");
            testing::RecoverTransaction(f.root); f.OriginalRestored();
        }
        {
            Fixture f(3);
            HANDLE locked = CreateFileW((f.root / L"psftp.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (locked == INVALID_HANDLE_VALUE) throw std::runtime_error("Windows lock fixture failed");
            bool rejected = false;
            try { testing::ApplyTransaction(f.root, f.stage); } catch (...) { rejected = true; }
            CloseHandle(locked);
            Check(rejected, "real Windows deny-delete file handle blocks replacement");
            Check(!fs::exists(f.Journal()), "locked target fails preflight before any transaction starts");
            f.OriginalRestored();
        }
        {
            Fixture f(4);
            Reject([&] { testing::ApplyTransaction(f.root, f.stage, 1); }, "recovery integrity fixture created");
            const auto before = Sha256File(f.root / L"serialctl.exe");
            Write(f.stage / L"backup" / L"serialctl.exe", "tampered-backup");
            Reject([&] { testing::RecoverTransaction(f.root); }, "damaged backup rejects recovery before any restoration");
            Check(Sha256File(f.root / L"serialctl.exe") == before && fs::exists(f.Journal()), "failed recovery retains journal and current executable");
        }
        {
            Fixture f(5);
            Write(f.Journal(), "SERIALCTL-UPDATE-1\n00000000000000000000000000000005\nV1.0.0\nV9.0.0\ninstalling\n../update.ini\t1\t0\t" + std::string(64, '0') + "\n");
            Reject([&] { testing::RecoverTransaction(f.root); }, "forged journal cannot target configuration or traverse directories");
            f.OriginalRestored();
        }
        Key key;
        for (bool embedded : {false, true}) {
            Fixture f(embedded ? 15 : 6); f.ExecutablePayload(key, embedded);
            Process parent(f.root / L"serialctl.exe", L"--test-update-parent " + serialctl::QuoteCommandLineArgument(f.stage.wstring()));
            Check(parent.Wait() == 0, "signed worker validates and parent exits normally");
            Check(WaitFile(f.root / L"healthy.txt"), "real replacement process creates window and completes health ACK");
            for (int i = 0; i < 100 && fs::exists(f.Journal()); ++i) Sleep(25);
            Check(!fs::exists(f.Journal()) && Read(f.root / L"VERSION") == "PROJECT_VERSION=V9.0.0\nCPP_VERSION=V9.0.0\n" && Read(f.root / L"README.md") == "new-readme", "healthy update commits new files and finalizes journal");
            Check(Read(f.root / L"update.ini") == "private user update URL", "full worker preserves update.ini");
        }
        for (unsigned i = 7; i <= 9; ++i) {
            Fixture f(i == 9 ? 14 : i); f.ExecutablePayload(key, true);
            const auto oldHash = Sha256File(f.root / L"serialctl.exe");
            Write(f.root / L"health-mode.txt", i == 7 ? "exit" : i == 8 ? "hang" : "version-mismatch");
            Process parent(f.root / L"serialctl.exe", L"--test-update-parent " + serialctl::QuoteCommandLineArgument(f.stage.wstring()));
            Check(parent.Wait() == 0, "failed-health fixture parent exits normally");
            Check(WaitFile(f.root / L"recovered.txt"), i == 7 ? "replacement early exit triggers rollback and old process restart" :
                i == 8 ? "replacement health timeout terminates only own child then restarts old process" :
                "replacement compiled-version mismatch refuses health ACK and restores original application");
            Check(Sha256File(f.root / L"serialctl.exe") == oldHash && Read(f.root / L"VERSION") == "V1.0.0\n" && Read(f.root / L"plink.exe") == "old-plink" &&
                Read(f.root / L"psftp.exe") == "old-psftp" && !fs::exists(f.root / L"README.md"), "health failure restores all original payload bytes");
            Check(Read(f.root / L".serialctl-update" / L"failed-version.txt") == "V9.0.0\n", "failed version persists to avoid update loop");
        }
        {
            Fixture f(9); f.ExecutablePayload(key);
            Reject([&] { testing::ApplyTransaction(f.root, f.stage, 2); }, "interrupted-install state prepared without worker cleanup");
            Process nextStartup(f.root / L"serialctl.exe", L"--test-normal-start");
            Check(nextStartup.Wait() == 0 && WaitFile(f.root / L"recovered.txt"), "next real process startup launches recovery before GUI initialization");
            Check(Read(f.root / L"VERSION") == "V1.0.0\n" && Read(f.root / L"plink.exe") == "old-plink" && !fs::exists(f.Journal()), "persisted journal restores previous payload set on next launch");
        }
        {
            Fixture f(10); f.ExecutablePayload(key);
            fs::copy_file(Self(), f.root / L"renamed-instance.exe");
            Process holder(f.root / L"renamed-instance.exe", L"--test-hold");
            Check(WaitFile(f.root / L"holder-ready.txt"), "renamed same-directory instance holds runtime registration");
            Process parent(f.root / L"serialctl.exe", L"--test-update-parent " + serialctl::QuoteCommandLineArgument(f.stage.wstring()));
            Check(parent.Wait() == 36, "worker rejects another live instance even with renamed executable");
            Check(WaitForSingleObject(holder.process, 0) == WAIT_TIMEOUT && Read(f.root / L"VERSION") == "V1.0.0\n", "other instance remains running and original files remain unchanged");
        }
        {
            Fixture f(11); f.ExecutablePayload(key);
            Junction alias(fs::path(f.root.wstring() + L"-junction"), f.root);
            Process holder(alias.path / L"serialctl.exe", L"--test-junction-hold");
            Check(WaitFile(f.root / L"holder-ready.txt"), "ordinary startup through real junction succeeds with automatic update disabled");
            Process parent(f.root / L"serialctl.exe", L"--test-update-parent " + serialctl::QuoteCommandLineArgument(f.stage.wstring()));
            Check(parent.Wait() == 36, "resolved installation identity detects live instance through junction alias");
            Check(WaitForSingleObject(holder.process, 0) == WAIT_TIMEOUT && Read(f.root / L"VERSION") == "V1.0.0\n", "junction instance is not terminated and payload remains unchanged");
        }
        {
            Fixture f(12); f.ExecutablePayload(key);
            fs::copy_file(Self(), f.root / L"long-renamed-instance.exe");
            wchar_t shortPath[32768]{};
            const DWORD count = GetShortPathNameW((f.root / L"long-renamed-instance.exe").c_str(), shortPath, 32768);
            if (count && count < 32768 && _wcsicmp(shortPath, (f.root / L"long-renamed-instance.exe").c_str()) != 0) {
                Process holder(fs::path(shortPath), L"--test-hold");
                Check(WaitFile(f.root / L"holder-ready.txt"), "real 8.3-path instance starts and registers");
                Process parent(f.root / L"serialctl.exe", L"--test-update-parent " + serialctl::QuoteCommandLineArgument(f.stage.wstring()));
                Check(parent.Wait() == 36, "long-path updater detects renamed live instance launched through 8.3 alias");
                Check(WaitForSingleObject(holder.process, 0) == WAIT_TIMEOUT && Read(f.root / L"VERSION") == "V1.0.0\n", "8.3 instance remains running without replacement");
            } else std::cout << "[SKIP] filesystem did not provide a distinct 8.3 alias; no 8.3 runtime assertion claimed\n";
        }
        {
            Fixture f(13); f.ExecutablePayload(key);
            const auto originalHash = Sha256File(f.root / L"serialctl.exe");
            Process parent(f.root / L"serialctl.exe", L"--test-refused-update-parent " + serialctl::QuoteCommandLineArgument(f.stage.wstring()));
            Check(parent.Wait() == 36, "ready worker remains alive after caller refuses launch without terminating helper");
            Check(WaitText(f.stage / L"update.log", "Update caller did not authorize installation"), "unapproved real helper rejects subsequent normal parent exit");
            Check(!fs::exists(f.Journal()) && !fs::exists(f.root / L"healthy.txt") && !fs::exists(f.root / L"recovered.txt") &&
                Sha256File(f.root / L"serialctl.exe") == originalHash && Read(f.root / L"VERSION") == "V1.0.0\n" &&
                Read(f.root / L"plink.exe") == "old-plink" && Read(f.root / L"psftp.exe") == "old-psftp" && !fs::exists(f.root / L"README.md"),
                "unapproved helper never starts a transaction, changes payloads, or launches replacement processes");
            fs::remove(f.stage / L"update.log");
            Check(Read(f.root / L".serialctl-update" / L"last-install-error.log").find("Injected launch refusal") != std::string::npos,
                "launch failure evidence survives deletion of disposable staged log");
        }
        std::cout << "BOUNDARY: actual Win32 file/process/health/journal operations; ephemeral test trust only; replacement application is this fixture, not SerialCtl GUI; no Win7/hardware/power-loss claim.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << std::endl; return 1; }
}
