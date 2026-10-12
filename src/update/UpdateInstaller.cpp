#include "UpdateInstaller.h"
#include "UpdateManifest.h"
#include "Win32Helpers.h"

#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace serialctl::update {
namespace {
namespace fs = std::filesystem;
constexpr DWORD kParentExitTimeout = 60000;
constexpr DWORD kStartupTimeout = 60000;
constexpr wchar_t kRootName[] = L".serialctl-update";
constexpr wchar_t kJournalName[] = L"transaction.txt";
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE release() { HANDLE h = value; value = nullptr; return h; }
};
struct MutexLock {
    Handle handle;
    bool held = false;
    ~MutexLock() { if (held) ReleaseMutex(handle.value); }
};
[[noreturn]] void Fail(const char* what) {
    throw std::runtime_error(std::string(what) + " (Win32=" + std::to_string(GetLastError()) + ")");
}
std::wstring Wide(const std::string& s) { return MultiByteToWide(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), CP_UTF8); }
std::string Utf8(const std::wstring& s) { return WideToMultiByte(s, CP_UTF8); }
fs::path ModulePath() {
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) Fail("Cannot locate application image");
    return fs::path(std::wstring(path.data(), n));
}
fs::path IdentityPath(const fs::path& path) {
    // A lexical pathname is not an installation identity: an 8.3 alias or a
    // junction must share the lock with its long, resolved pathname. Keep this
    // distinct from SafeTree: resolution must never hide a forbidden reparse
    // point when deciding whether files may actually be replaced.
    Handle file(CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) Fail("Cannot resolve installation identity");
    std::vector<wchar_t> value(32768);
    DWORD length = GetFinalPathNameByHandleW(file.value, value.data(), static_cast<DWORD>(value.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= value.size()) Fail("Cannot resolve installation identity");
    return fs::path(std::wstring(value.data(), length)).lexically_normal();
}
bool SamePath(const fs::path& a, const fs::path& b) {
    return _wcsicmp(IdentityPath(a).c_str(), IdentityPath(b).c_str()) == 0;
}
void SafeTree(const fs::path& path, bool exists = true) {
    fs::path current;
    for (const auto& part : fs::absolute(path).lexically_normal()) {
        current /= part;
        DWORD attr = GetFileAttributesW(current.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            DWORD code = GetLastError();
            if (!exists && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)) continue;
            Fail("Update path is unavailable");
        }
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Update paths must not contain junctions or symbolic links");
    }
}
bool TokenValid(const std::string& token) {
    return token.size() == 32 && std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string StageToken(const fs::path& installation, const fs::path& stage) {
    std::string token = stage.filename().string();
    if (!TokenValid(token) || !SamePath(stage.parent_path(), installation / kRootName))
        throw std::runtime_error("Staging directory does not belong to this installation");
    SafeTree(stage);
    return token;
}
std::wstring MutexName(const fs::path& installation) {
    // Hashing only selects a lock; a collision fails closed, never selects files.
    std::wstring normalized = IdentityPath(installation).wstring();
    std::uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : normalized) { hash ^= static_cast<unsigned>(towlower(c)); hash *= 1099511628211ull; }
    std::wostringstream out;
    out << L"Global\\SerialCtl.Update." << std::hex << hash;
    return out.str();
}
std::wstring InstanceName(const fs::path& installation, DWORD pid) {
    return MutexName(installation) + L".Instance." + std::to_wstring(pid);
}
void LockInstallation(const fs::path& installation, MutexLock& lock) {
    lock.handle.value = CreateMutexW(nullptr, FALSE, MutexName(installation).c_str());
    if (!lock.handle.value) Fail("Cannot open installation update lock");
    DWORD result = WaitForSingleObject(lock.handle.value, 0);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
        throw std::runtime_error("Another instance is starting or an update is in progress; please retry after it finishes");
    lock.held = true;
}
std::wstring EventName(const std::string& token, const wchar_t* suffix) {
    return L"Local\\SerialCtl.Update." + Wide(token) + suffix;
}
std::uint64_t CreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) Fail("Cannot identify process creation time");
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
fs::path ProcessImage(HANDLE process) {
    std::vector<wchar_t> value(32768); DWORD size = static_cast<DWORD>(value.size());
    if (!QueryFullProcessImageNameW(process, 0, value.data(), &size)) Fail("Cannot identify process image");
    return fs::path(std::wstring(value.data(), size));
}
void RequireNoOtherProcesses(const fs::path& installation, DWORD ignorePid = 0) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) Fail("Cannot enumerate running processes");
    PROCESSENTRY32W item{}; item.dwSize = sizeof(item);
    if (!Process32FirstW(snapshot.value, &item)) Fail("Cannot enumerate running processes");
    do {
        if (item.th32ProcessID == GetCurrentProcessId() || item.th32ProcessID == ignorePid) continue;
        Handle instance(OpenMutexW(SYNCHRONIZE, FALSE, InstanceName(installation, item.th32ProcessID).c_str()));
        if (instance.value || GetLastError() == ERROR_ACCESS_DENIED)
            throw std::runtime_error("Another application instance from this installation is still running; close it before updating");
        if (_wcsicmp(item.szExeFile, L"serialctl.exe") && _wcsicmp(item.szExeFile, L"plink.exe") && _wcsicmp(item.szExeFile, L"psftp.exe")) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, item.th32ProcessID));
        if (!process.value) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue; // process already exited
            throw std::runtime_error("Cannot inspect another SerialCtl/PuTTY process; update postponed");
        }
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) continue;
        if (SamePath(ProcessImage(process.value).parent_path(), installation))
            throw std::runtime_error("Another SerialCtl, CMD bridge, SSH or SFTP process from this installation is still running; close it before updating");
    } while (Process32NextW(snapshot.value, &item));
    if (GetLastError() != ERROR_NO_MORE_FILES) Fail("Process enumeration was incomplete");
}
void WriteDurable(const fs::path& destination, const std::string& content) {
    SafeTree(destination.parent_path()); SafeTree(destination, false);
    fs::path temporary = destination; temporary += L".tmp";
    SafeTree(temporary, false);
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) Fail("Cannot write update journal");
    DWORD written = 0;
    if (!WriteFile(file.value, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) || written != content.size() || !FlushFileBuffers(file.value))
        Fail("Cannot flush update journal");
    CloseHandle(file.release());
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) Fail("Cannot commit update journal");
}
std::string ReadSmall(const fs::path& path, std::size_t limit) {
    SafeTree(path);
    if (fs::file_size(path) > limit) throw std::runtime_error("Update journal is too large");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read update journal");
    return std::string(std::istreambuf_iterator<char>(input), {});
}
void Log(const fs::path& stage, const std::string& message) noexcept {
    try {
        SafeTree(stage / L"update.log", false);
        std::ofstream log(stage / L"update.log", std::ios::binary | std::ios::app);
        SYSTEMTIME time{}; GetSystemTime(&time);
        log << time.wYear << '-' << time.wMonth << '-' << time.wDay << 'T' << time.wHour << ':' << time.wMinute << ':' << time.wSecond << "Z " << message << '\n';
    } catch (...) {}
}
struct Entry { std::string name; bool existed = false; std::uintmax_t size = 0; std::string hash; };
struct Journal { std::string token, previousVersion, version, phase; std::vector<Entry> entries; };
void SaveJournal(const fs::path& installation, const Journal& journal) {
    std::ostringstream text;
    text << "SERIALCTL-UPDATE-1\n" << journal.token << '\n' << journal.previousVersion << '\n' << journal.version << '\n' << journal.phase << '\n';
    for (const auto& entry : journal.entries) text << entry.name << '\t' << entry.existed << '\t' << entry.size << '\t' << entry.hash << '\n';
    WriteDurable(installation / kRootName / kJournalName, text.str());
}
Journal ReadJournal(const fs::path& installation) {
    std::istringstream data(ReadSmall(installation / kRootName / kJournalName, 65536));
    std::string magic; Journal journal;
    std::getline(data, magic); std::getline(data, journal.token); std::getline(data, journal.previousVersion); std::getline(data, journal.version); std::getline(data, journal.phase);
    if (magic != "SERIALCTL-UPDATE-1" || !TokenValid(journal.token) || CompareVersions(journal.version, journal.previousVersion) <= 0 ||
        (journal.phase != "prepared" && journal.phase != "installing" && journal.phase != "awaiting" && journal.phase != "committed" && journal.phase != "rolling-back"))
        throw std::runtime_error("Invalid update recovery journal; manual recovery required");
    std::string line;
    while (std::getline(data, line)) {
        std::istringstream row(line); Entry entry; unsigned existed = 2;
        if (!(row >> entry.name >> existed >> entry.size >> entry.hash) || existed > 1 || !IsAllowedUpdateFile(entry.name) || entry.hash.size() != 64 ||
            !std::all_of(entry.hash.begin(), entry.hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
            std::any_of(journal.entries.begin(), journal.entries.end(), [&](const Entry& other) { return other.name == entry.name; }))
            throw std::runtime_error("Invalid update recovery file record");
        std::string extra; if (row >> extra) throw std::runtime_error("Unexpected recovery record data");
        if (!existed && (entry.size != 0 || entry.hash != std::string(64, '0'))) throw std::runtime_error("Invalid absent-file recovery record");
        entry.existed = existed != 0; journal.entries.push_back(entry);
    }
    if (journal.entries.empty() || journal.entries.size() > kMaxUpdateFiles) throw std::runtime_error("Invalid recovery file count");
    StageToken(installation, installation / kRootName / Wide(journal.token));
    return journal;
}
void CopyChecked(const fs::path& from, const fs::path& to) {
    SafeTree(from); SafeTree(to, false);
    if (!CopyFileW(from.c_str(), to.c_str(), TRUE)) Fail("Cannot copy update file");
    Handle file(CreateFileW(to.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE || !FlushFileBuffers(file.value)) Fail("Cannot flush update file");
}
void ReplaceChecked(const fs::path& source, const fs::path& destination, const std::string& token, std::uintmax_t size, const std::string& hash) {
    SafeTree(destination, false);
    fs::path temporary = destination.parent_path() / (L".serialctl-" + Wide(token) + L"-" + destination.filename().wstring());
    SafeTree(temporary, false);
    if (fs::exists(temporary) && !DeleteFileW(temporary.c_str())) Fail("Cannot remove incomplete update copy");
    CopyChecked(source, temporary);
    if (fs::file_size(temporary) != size || Sha256File(temporary) != hash) throw std::runtime_error("Copied file failed integrity verification");
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) Fail("Cannot replace application file (possibly in use)");
    if (fs::file_size(destination) != size || Sha256File(destination) != hash) throw std::runtime_error("Installed file failed integrity verification");
}
Journal PrepareTransaction(const fs::path& installation, const fs::path& stage, const Manifest& manifest, const std::string& previousVersion) {
    Journal journal{StageToken(installation, stage), previousVersion, manifest.version, "prepared", {}};
    fs::path backup = stage / L"backup";
    if (fs::exists(backup)) throw std::runtime_error("Update backup already exists; recovery must complete first");
    fs::create_directory(backup); SafeTree(backup);
    for (const auto& file : manifest.files) {
        fs::path target = installation / Wide(file.name); SafeTree(target, false);
        Entry entry; entry.name = file.name; entry.existed = fs::exists(target);
        entry.hash = std::string(64, '0');
        if (entry.existed) {
            if (!fs::is_regular_file(target)) throw std::runtime_error("Existing update target is not a file");
            entry.size = fs::file_size(target); entry.hash = Sha256File(target);
            CopyChecked(target, backup / Wide(file.name));
            if (fs::file_size(backup / Wide(file.name)) != entry.size || Sha256File(backup / Wide(file.name)) != entry.hash)
                throw std::runtime_error("Backup integrity verification failed");
        }
        journal.entries.push_back(entry);
    }
    SaveJournal(installation, journal); return journal;
}
void RequireReplaceableTargets(const fs::path& installation, const Manifest& manifest) {
    // Check every existing target before backing up or replacing the first one.
    // A third-party deny-delete handle is therefore an unchanged-installation
    // failure. Races after this preflight still use journalled rollback.
    for (const auto& file : manifest.files) {
        const auto target = installation / Wide(file.name); SafeTree(target, false);
        if (!fs::exists(target)) continue;
        const DWORD attributes = GetFileAttributesW(target.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)))
            throw std::runtime_error("An application file cannot be replaced; update postponed");
        Handle probe(CreateFileW(target.c_str(), GENERIC_READ | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (probe.value == INVALID_HANDLE_VALUE) Fail("An application file is in use or cannot be replaced; update postponed");
    }
}
void ReplaceTransaction(const fs::path& installation, const fs::path& stage, const Manifest& manifest, Journal& journal, int failAfter = -1) {
    journal.phase = "installing"; SaveJournal(installation, journal);
    std::vector<UpdateFile> files = manifest.files;
    // The executable changes last, reducing the interrupted-install startup gap.
    std::stable_sort(files.begin(), files.end(), [](const UpdateFile& a, const UpdateFile& b) { return a.name != "serialctl.exe" && b.name == "serialctl.exe"; });
    int count = 0;
    for (const auto& file : files) {
        if (failAfter >= 0 && count == failAfter) throw std::runtime_error("Injected fixture replacement failure");
        ReplaceChecked(stage / L"files" / Wide(file.name), installation / Wide(file.name), journal.token, file.size, file.sha256);
        ++count;
    }
    journal.phase = "awaiting"; SaveJournal(installation, journal);
}
void Recover(const fs::path& installation, Journal& journal) {
    if (journal.phase == "committed") {
        if (!DeleteFileW((installation / kRootName / kJournalName).c_str())) Fail("Cannot finalize confirmed update journal");
        return;
    }
    const fs::path stage = installation / kRootName / Wide(journal.token);
    const fs::path backup = stage / L"backup"; SafeTree(backup);
    // Validate every original before changing anything. Recovery never consumes
    // unverified remote payloads; it restores the locally captured pre-update set.
    for (const auto& entry : journal.entries) if (entry.existed) {
        const auto path = backup / Wide(entry.name); SafeTree(path);
        if (fs::file_size(path) != entry.size || Sha256File(path) != entry.hash) throw std::runtime_error("Recovery backup is damaged; manual recovery required");
    }
    journal.phase = "rolling-back"; SaveJournal(installation, journal);
    for (auto it = journal.entries.rbegin(); it != journal.entries.rend(); ++it) {
        const auto target = installation / Wide(it->name);
        if (it->existed) {
            SafeTree(target, false);
            if (!fs::exists(target) || fs::file_size(target) != it->size || Sha256File(target) != it->hash)
                ReplaceChecked(backup / Wide(it->name), target, journal.token, it->size, it->hash);
        }
        else { SafeTree(target, false); if (fs::exists(target) && !DeleteFileW(target.c_str())) Fail("Cannot remove newly installed file during rollback"); }
    }
    WriteDurable(installation / kRootName / L"failed-version.txt", journal.version + "\n");
    if (!DeleteFileW((installation / kRootName / kJournalName).c_str())) Fail("Cannot complete update rollback");
    Log(stage, "ROLLBACK completed; failed version suppressed: " + journal.version);
}
std::vector<std::wstring> Parse(const std::wstring& parameters) {
    int count = 0; LPWSTR* args = CommandLineToArgvW((L"serialctl.exe " + parameters).c_str(), &count);
    if (!args) Fail("Cannot parse update command line");
    std::vector<std::wstring> result(args + 1, args + count); LocalFree(args); return result;
}
std::uint64_t Number(const std::wstring& text) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) throw std::runtime_error("Invalid update process identifier");
    std::size_t end = 0; auto value = std::stoull(text, &end);
    if (end != text.size()) throw std::runtime_error("Invalid update process identifier");
    return value;
}
void StartProcess(const fs::path& executable, const std::wstring& parameters, const fs::path& directory, PROCESS_INFORMATION& process) {
    std::wstring line = QuoteCommandLineArgument(executable.wstring()) + L" " + parameters;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup, &process)) Fail("Cannot start update process");
}
std::wstring ParentArgs() {
    return std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(CreationTime(GetCurrentProcess())) + L" ";
}
Manifest VerifiedStage(const fs::path& stage, const std::string& previousVersion, bool fixture) {
#ifdef SERIALCTL_UPDATE_TESTING
    if (fixture) {
        const auto key = ReadSmall(stage / L"test-public-key.bin", 512);
        return testing::VerifyStagedManifest(stage, previousVersion, std::vector<unsigned char>(key.begin(), key.end()));
    }
#else
    (void)fixture;
#endif
    return VerifyStagedManifest(stage, previousVersion);
}
bool LaunchWorker(const fs::path& installation, const fs::path& stage, const std::string& previousVersion, bool recover, std::wstring& error, bool fixture = false) {
    try {
        const auto token = StageToken(installation, stage);
        Handle ready(CreateEventW(nullptr, TRUE, FALSE, EventName(token, L".ready").c_str()));
        if (!ready.value || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("Update readiness channel already exists");
        const auto worker = stage / L"update-worker.exe";
        SafeTree(worker, false);
        if (fs::exists(worker) && !DeleteFileW(worker.c_str())) Fail("Cannot remove old update worker");
        CopyChecked(ModulePath(), worker);
        if (Sha256File(worker) != Sha256File(ModulePath())) throw std::runtime_error("Update worker copy failed integrity verification");
        PROCESS_INFORMATION process{};
        const std::wstring mode = fixture ? L"--serialctl-update-test-worker " : L"--serialctl-update-worker ";
        StartProcess(worker, mode + ParentArgs() + QuoteCommandLineArgument(Wide(previousVersion)) + L" " + (recover ? L"recover" : L"install"), installation, process);
        Handle child(process.hProcess), thread(process.hThread);
        HANDLE waits[] = {ready.value, child.value};
        if (WaitForMultipleObjects(2, waits, FALSE, 15000) != WAIT_OBJECT_0) {
            // The GUI is still alive, so this child cannot yet have modified any
            // application files. Stop only the helper we just created before
            // returning stage ownership to the caller for cleanup.
            if (WaitForSingleObject(child.value, 0) != WAIT_OBJECT_0) {
                if (!TerminateProcess(child.value, 72) || WaitForSingleObject(child.value, 10000) != WAIT_OBJECT_0)
                    throw std::runtime_error("Unready update helper could not be stopped; keep its staging directory");
            }
            throw std::runtime_error("Update worker did not become ready; see the staged update.log");
        }
        return true;
    } catch (const std::exception& ex) { error = Wide(ex.what()); return false; }
}
void RestartOriginal(const fs::path& installation) {
    PROCESS_INFORMATION process{}; StartProcess(installation / L"serialctl.exe", L"--serialctl-update-recovered", installation, process);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
int Worker(const std::vector<std::wstring>& args, bool fixture = false) {
    const auto stage = ModulePath().parent_path();
    bool parentExited = false;
    bool restarted = false;
    fs::path installation;
    std::string failedVersion;
    try {
        if (args.size() != 5 || args[0] != (fixture ? L"--serialctl-update-test-worker" : L"--serialctl-update-worker") || (args[4] != L"install" && args[4] != L"recover")) throw std::runtime_error("Invalid internal update command");
        const auto pid64 = Number(args[1]); if (pid64 > MAXDWORD || !pid64) throw std::runtime_error("Invalid parent PID");
        Handle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid64)));
        if (!parent.value || CreationTime(parent.value) != Number(args[2])) throw std::runtime_error("Update parent no longer matches");
        const auto image = ProcessImage(parent.value);
        if (_wcsicmp(image.filename().c_str(), L"serialctl.exe")) throw std::runtime_error("Update parent is not SerialCtl");
        installation = image.parent_path(); SafeTree(installation);
        const auto token = StageToken(installation, stage);
        const std::string previousVersion = Utf8(args[3]); CompareVersions(previousVersion, previousVersion);
        MutexLock lock; LockInstallation(installation, lock);
        const bool recovery = args[4] == L"recover";
        Manifest manifest; Journal journal;
        if (recovery) {
            journal = ReadJournal(installation);
            if (journal.token != token || journal.previousVersion != previousVersion) throw std::runtime_error("Recovery worker and journal disagree");
        } else {
            if (fs::exists(installation / kRootName / kJournalName)) throw std::runtime_error("An unfinished update must recover first");
            manifest = VerifiedStage(stage, previousVersion, fixture);
            failedVersion = manifest.version;
        }
        RequireNoOtherProcesses(installation, static_cast<DWORD>(pid64));
        Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(token, L".ready").c_str()));
        if (!ready.value || !SetEvent(ready.value)) throw std::runtime_error("Update caller readiness channel disappeared");
        Log(stage, recovery ? "RECOVERY waiting for parent exit" : "VERIFIED waiting for parent exit");
        if (WaitForSingleObject(parent.value, kParentExitTimeout) != WAIT_OBJECT_0) throw std::runtime_error("Parent did not exit normally; installation cancelled");
        parentExited = true;
        RequireNoOtherProcesses(installation);
        if (recovery) {
            Recover(installation, journal);
            ReleaseMutex(lock.handle.value); lock.held = false;
            RestartOriginal(installation); restarted = true; return 0;
        }
        // Downloads and signatures are checked again after waiting for shutdown.
        manifest = VerifiedStage(stage, previousVersion, fixture);
        bool journalCreated = false;
        Handle replacement;
        try {
            RequireReplaceableTargets(installation, manifest);
            journal = PrepareTransaction(installation, stage, manifest, previousVersion); journalCreated = true;
            ReplaceTransaction(installation, stage, manifest, journal);
            Handle acknowledgment(CreateEventW(nullptr, TRUE, FALSE, EventName(token, L".healthy").c_str()));
            if (!acknowledgment.value || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("Update health channel already exists");
            Handle committed(CreateEventW(nullptr, TRUE, FALSE, EventName(token, L".committed").c_str()));
            if (!committed.value || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("Update commit channel already exists");
            PROCESS_INFORMATION process{};
            StartProcess(installation / L"serialctl.exe", L"--serialctl-update-health " + ParentArgs() + Wide(token), installation, process);
            replacement.value = process.hProcess;
            Handle thread(process.hThread);
            HANDLE waits[] = {replacement.value, acknowledgment.value};
            DWORD wait = WaitForMultipleObjects(2, waits, FALSE, fixture ? 2500 : kStartupTimeout);
            if (wait != WAIT_OBJECT_0 + 1)
                throw std::runtime_error("Replacement application did not confirm a healthy main window");
            journal.phase = "committed";
            try { SaveJournal(installation, journal); } catch (...) { journal.phase = "awaiting"; throw; }
            if (!SetEvent(committed.value)) Log(stage, "WARNING: could not signal committed update");
            Log(stage, "COMMITTED " + manifest.version);
            try { Recover(installation, journal); } // committed journal: finalize only
            catch (const std::exception& ex) { Log(stage, std::string("Finalization deferred: ") + ex.what()); }
            return 0;
        } catch (const std::exception& ex) {
            Log(stage, std::string("INSTALL FAILED: ") + ex.what());
            // Only this worker's replacement child may be terminated. Old and
            // unrelated running applications are never killed.
            if (replacement.value && WaitForSingleObject(replacement.value, 0) != WAIT_OBJECT_0) {
                if (!TerminateProcess(replacement.value, 71) || WaitForSingleObject(replacement.value, 10000) != WAIT_OBJECT_0)
                    throw std::runtime_error("Failed update child could not exit; recovery is blocked safely");
            }
            if (journalCreated) {
                RequireNoOtherProcesses(installation);
                Recover(installation, journal);
            }
            ReleaseMutex(lock.handle.value); lock.held = false;
            // A pre-journal backup failure also needs loop suppression. The
            // recovered startup argument works even if the disk cannot store it.
            try { WriteDurable(installation / kRootName / L"failed-version.txt", manifest.version + "\n"); }
            catch (const std::exception& markerError) { Log(stage, std::string("Cannot persist failure marker: ") + markerError.what()); }
            RestartOriginal(installation); restarted = true;
            throw;
        }
    } catch (const std::exception& ex) {
        Log(stage, std::string("ERROR: ") + ex.what());
        // Failures between normal parent exit and journal preparation leave the
        // original set untouched. Restart it as well, with this launch's update
        // check suppressed even when failure-marker writes cannot succeed.
        if (parentExited && !restarted && !installation.empty()) {
            try {
                if (!fs::exists(installation / kRootName / kJournalName)) {
                    if (!failedVersion.empty()) {
                        try { WriteDurable(installation / kRootName / L"failed-version.txt", failedVersion + "\n"); } catch (...) {}
                    }
                    RestartOriginal(installation); restarted = true;
                }
            } catch (const std::exception& restartError) { Log(stage, std::string("Cannot restart original application: ") + restartError.what()); }
        }
        if (parentExited && !restarted && !fixture) {
            const auto message = L"自动更新未能完成或恢复。请保留更新目录，并从已验证的正式包恢复程序文件。\n\n" +
                Wide(ex.what()) + L"\n日志：" + (stage / L"update.log").wstring();
            MessageBoxW(nullptr, message.c_str(), L"SerialCtl 更新失败", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}
} // namespace

bool HandleUpdateCommandLine(const std::wstring& parameters, int& exitCode) {
    bool fixture = false;
#ifdef SERIALCTL_UPDATE_TESTING
    fixture = parameters.find(L"--serialctl-update-test-worker") == 0;
#endif
    if (!fixture && parameters.find(L"--serialctl-update-worker") != 0) return false;
    try { exitCode = Worker(Parse(parameters), fixture); }
    catch (...) { exitCode = 1; }
    return true;
}
StartupGuard::~StartupGuard() {
    if (instance_) CloseHandle(instance_);
    if (acknowledgment_) CloseHandle(acknowledgment_);
    if (committed_) CloseHandle(committed_);
    if (worker_) CloseHandle(worker_);
    if (mutex_) { ReleaseMutex(mutex_); CloseHandle(mutex_); }
}
StartupAction StartupGuard::Begin(const std::wstring& parameters, std::wstring& error) {
    try {
        const auto installation = ModulePath().parent_path();
        const auto args = Parse(parameters);
        recovered_ = args.size() == 1 && args[0] == L"--serialctl-update-recovered";
        if (!args.empty() && args[0] == L"--serialctl-update-health") {
            if (args.size() != 4 || !TokenValid(Utf8(args[3]))) throw std::runtime_error("Invalid update health arguments");
            const auto pid = Number(args[1]); if (!pid || pid > MAXDWORD) throw std::runtime_error("Invalid update worker PID");
            Handle worker(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
            if (!worker.value || CreationTime(worker.value) != Number(args[2]) || WaitForSingleObject(worker.value, 0) != WAIT_TIMEOUT ||
                !SamePath(ProcessImage(worker.value), installation / kRootName / args[3] / L"update-worker.exe"))
                throw std::runtime_error("Update health worker does not match this installation");
            SafeTree(installation);
            Journal journal = ReadJournal(installation);
            if (journal.token != Utf8(args[3]) || journal.phase != "awaiting") throw std::runtime_error("Update health journal does not match");
            acknowledgment_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(journal.token, L".healthy").c_str());
            if (!acknowledgment_) Fail("Cannot open update health confirmation channel");
            committed_ = OpenEventW(SYNCHRONIZE, FALSE, EventName(journal.token, L".committed").c_str());
            if (!committed_) Fail("Cannot open update commit confirmation channel");
            worker_ = worker.release();
            instance_ = CreateMutexW(nullptr, FALSE, InstanceName(installation, GetCurrentProcessId()).c_str());
            if (!instance_) Fail("Cannot register application instance");
            return StartupAction::Continue;
        }
        MutexLock lock; LockInstallation(installation, lock);
        if (fs::exists(installation / kRootName / kJournalName)) {
            SafeTree(installation);
            auto journal = ReadJournal(installation);
            if (journal.phase == "committed") Recover(installation, journal);
            else {
                // A worker needs the lock; this process has not initialized any
                // services yet and exits as soon as recovery readiness is signaled.
                ReleaseMutex(lock.handle.value); lock.held = false;
                if (!LaunchWorker(installation, installation / kRootName / Wide(journal.token), journal.previousVersion, true, error)) return StartupAction::Failed;
                return StartupAction::Exit;
            }
        }
        // Legacy installs under junctions may continue to run. Automatic
        // replacement through such aliases is disabled; unfinished transactions
        // above still fail closed and require controlled recovery.
        try { SafeTree(installation); } catch (...) { recovered_ = true; }
        instance_ = CreateMutexW(nullptr, FALSE, InstanceName(installation, GetCurrentProcessId()).c_str());
        if (!instance_) Fail("Cannot register application instance");
        mutex_ = lock.handle.release(); lock.held = false;
        return StartupAction::Continue;
    } catch (const std::exception& ex) { error = Wide(ex.what()); return StartupAction::Failed; }
}
bool StartupGuard::Confirm(std::wstring& error) {
    if (acknowledgment_) {
        if (!SetEvent(acknowledgment_)) { error = L"无法确认更新后的主窗口启动成功。"; return false; }
        CloseHandle(acknowledgment_); acknowledgment_ = nullptr;
        HANDLE waits[] = {committed_, worker_};
        if (WaitForMultipleObjects(2, waits, FALSE, 15000) != WAIT_OBJECT_0) {
            error = L"更新事务未能提交；程序将退出并由更新器恢复旧版本。";
            return false;
        }
        CloseHandle(committed_); committed_ = nullptr;
        CloseHandle(worker_); worker_ = nullptr;
    }
    if (mutex_) { ReleaseMutex(mutex_); CloseHandle(mutex_); mutex_ = nullptr; }
    return true;
}
bool LaunchUpdateWorker(const fs::path& stagingDirectory, const std::string& currentVersion, std::wstring& error) {
    try {
        auto installation = ModulePath().parent_path();
        StageToken(installation, stagingDirectory);
        VerifyStagedManifest(stagingDirectory, currentVersion);
        return LaunchWorker(installation, stagingDirectory, currentVersion, false, error);
    } catch (const std::exception& ex) { error = Wide(ex.what()); return false; }
}

#ifdef SERIALCTL_UPDATE_TESTING
namespace testing {
void ApplyTransaction(const fs::path& installation, const fs::path& stage, int failAfter) {
    Manifest manifest; manifest.version = "V9.0.0";
    for (const auto& item : fs::directory_iterator(stage / L"files")) {
        const auto name = item.path().filename().string();
        if (!IsAllowedUpdateFile(name)) throw std::runtime_error("Invalid fixture filename");
        manifest.files.push_back({name, fs::file_size(item.path()), Sha256File(item.path()), {}});
    }
    RequireReplaceableTargets(installation, manifest);
    auto journal = PrepareTransaction(installation, stage, manifest, "V1.0.0");
    ReplaceTransaction(installation, stage, manifest, journal, failAfter);
}
void RecoverTransaction(const fs::path& installation) {
    auto journal = ReadJournal(installation); Recover(installation, journal);
}
bool LaunchSignedTestUpdate(const fs::path& stage, const std::string& currentVersion, std::wstring& error) {
    try {
        const auto installation = ModulePath().parent_path();
        StageToken(installation, stage);
        VerifiedStage(stage, currentVersion, true);
        return LaunchWorker(installation, stage, currentVersion, false, error, true);
    } catch (const std::exception& ex) { error = Wide(ex.what()); return false; }
}
}
#endif
} // namespace serialctl::update
