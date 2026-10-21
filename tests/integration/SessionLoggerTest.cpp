#include "SessionLogger.h"
#include "json.hpp"
using Json = nlohmann::json;
#include <wincrypt.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace serialctl;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace serialctl {
struct SessionLoggerTestAccess {
    static bool Drained(SessionLogger &logger) {
        std::unique_lock<std::mutex> lock(logger.mutex_);
        return logger.wake_.wait_for(lock, 5s, [&] {
            return logger.completed_ == logger.accepted_ || !logger.failure_.empty();
        }) && logger.failure_.empty();
    }
    static void FailCopyAfter(std::int64_t bytes) { SessionLogger::testCopyFailureAfter_.store(bytes); }
};
}
namespace {
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void Put(const fs::path &path, const std::string &data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    Check(bool(file), "write fixture");
}
std::string Read(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    Check(bool(file), "read fixture");
    return {std::istreambuf_iterator<char>(file), {}};
}
std::string Hash(const fs::path &path) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    Check(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE,
          "SHA256 provider");
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(provider, 0);
        throw std::runtime_error("SHA256 hash");
    }
    std::ifstream file(path, std::ios::binary);
    std::array<char, 65536> buffer{};
    bool ok = bool(file);
    while (ok && file) {
        file.read(buffer.data(), buffer.size());
        if (file.gcount()) ok = CryptHashData(hash, reinterpret_cast<BYTE *>(buffer.data()),
                                             static_cast<DWORD>(file.gcount()), 0) != FALSE;
    }
    ok = ok && file.eof();
    BYTE bytes[32]{};
    DWORD size = sizeof(bytes);
    ok = ok && CryptGetHashParam(hash, HP_HASHVAL, bytes, &size, 0) != FALSE && size == sizeof(bytes);
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    Check(ok, "SHA256 file");
    const char digits[] = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
void NoTemporary(const fs::path &target) {
    const auto prefix = target.filename().wstring() + L".serialctl-";
    for (const auto &entry : fs::directory_iterator(target.parent_path()))
        Check(entry.path().filename().wstring().find(prefix) != 0, "temporary file leaked");
}
struct Gate {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false, released = false;
    void Enter() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        wake.notify_all();
        wake.wait(lock, [&] { return released; });
    }
    bool Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        return wake.wait_for(lock, 5s, [&] { return entered; });
    }
    void Release() { std::lock_guard<std::mutex> lock(mutex); released = true; wake.notify_all(); }
};
struct ReleaseGate { Gate &gate; ~ReleaseGate() { gate.Release(); } };
}

int main() {
    Json evidence = {{"scope", "Native SessionLogger controlled local Win32 files; gated copy and injected write failure; no physical disk-full/SMB/Win7 claim"},
                     {"cases", Json::array()}};
    std::vector<fs::path> roots;
    fs::path directory;
    int exitCode = 0;
    try {
        directory = fs::temp_directory_path() / (L"SerialCtl-export-test-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(directory);
        SessionLogger logger;
        std::wstring error;
        Check(logger.Start(L"export-regression", error, true), "start logger");
        const auto root = fs::path(logger.Path());
        roots.push_back(root);
        const auto second = root.parent_path() / (root.stem().wstring() + L".part2" + root.extension().wstring());
        roots.push_back(second);
        const auto third = root.parent_path() / (root.stem().wstring() + L".part3" + root.extension().wstring());
        Bytes block(65536, 'A');
        for (int i = 0; i < 65 * 16; ++i) {
            logger.WriteRaw(block);
            if (i % 16 == 15) Check(SessionLoggerTestAccess::Drained(logger), "source writer drain");
        }
        const auto point = logger.CaptureExportPoint();
        Check(point.bytes == 65ull * 1024 * 1024 && fs::exists(second), "65 MiB source rotation");
        const auto target = directory / L"snapshot.log";
        Gate gate;
        std::uint64_t lastProgress = 0;
        auto copy = std::async(std::launch::async, [&] {
            return logger.SaveCopy(target.wstring(), point, error, nullptr,
                [&](std::uint64_t copied, std::uint64_t total) {
                    Check(total == point.bytes && copied >= lastProgress && copied <= total, "monotonic fixed progress");
                    lastProgress = copied;
                    if (copied == 65536) gate.Enter();
                });
        });
        ReleaseGate release{gate}; // always unblock before future destruction on any failure
        Check(gate.Wait(), "export reaches controlled slow-copy gate");
        const auto producerStart = Clock::now();
        logger.WriteRaw(Bytes(1024 * 1024, 'B'));
        const auto producerMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - producerStart).count();
        auto stop = std::async(std::launch::async, [&] { logger.Stop(); });
        const bool stopped = stop.wait_for(2s) == std::future_status::ready;
        gate.Release(); // also release before waiting on an unexpected Stop failure
        stop.get();
        Check(stopped && producerMs < 1000, "copy must not hold producer/Stop mutex during I/O");
        Check(copy.get(), "snapshot copy completes");
        Check(lastProgress == point.bytes && fs::file_size(target) == point.bytes, "fixed snapshot length");
        const auto hash = Hash(target);
        Check(hash == "293ac2ec51c4b810c496f147ebba14e5916f360a27cab012e7d8eccd1946f8ef", "snapshot SHA256 excludes later B bytes");
        evidence["cases"].push_back({{"name", "65 MiB rotation, controlled slow copy, concurrent producer and Stop"},
            {"bytes", point.bytes}, {"sha256", hash}, {"producer_ms", producerMs}, {"stop_while_copy_blocked", stopped},
            {"later_bytes_excluded", 1024 * 1024}, {"progress_final", lastProgress}});
        Check(!logger.SaveCopy(root.wstring(), point, error, nullptr), "protect source root");
        Check(!logger.SaveCopy(second.wstring(), point, error, nullptr), "protect existing part");
        Check(!logger.SaveCopy(third.wstring(), point, error, nullptr) && !fs::exists(third), "protect future part");
        evidence["cases"].push_back({{"name", "source root/current/future rotation protected"}});

        const auto old = directory / L"preserved.log";
        const std::string original = "ORIGINAL TARGET MUST SURVIVE";
        Put(old, original);
        std::atomic<bool> cancel{false};
        Check(!logger.SaveCopy(old.wstring(), point, error, &cancel,
            [&](std::uint64_t copied, std::uint64_t) { if (copied >= 65536) cancel.store(true); }), "cancel copy");
        Check(Read(old) == original && !error.empty(), "cancel preserves old target");
        NoTemporary(old);
        evidence["cases"].push_back({{"name", "cancel after partial copy; target preserved; temporary removed"}, {"cancel_after_bytes", 65536}});
        cancel.store(true);
        Check(!logger.SaveCopy(old.wstring(), point, error, &cancel) && Read(old) == original, "pre-cancel");
        SessionLoggerTestAccess::FailCopyAfter(65536);
        const bool failedCopy = !logger.SaveCopy(old.wstring(), point, error, nullptr);
        SessionLoggerTestAccess::FailCopyAfter(-1);
        Check(failedCopy && !error.empty() && logger.Error().empty() && Read(old) == original, "injected disk-full isolates export failure");
        NoTemporary(old);
        evidence["cases"].push_back({{"name", "controlled ERROR_DISK_FULL after 64 KiB; target preserved; logger healthy"}});
        Check(SetFileAttributesW(old.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "readonly fixture");
        const bool readonlyFailed = !logger.SaveCopy(old.wstring(), point, error, nullptr);
        SetFileAttributesW(old.c_str(), FILE_ATTRIBUTE_NORMAL);
        Check(readonlyFailed && Read(old) == original && logger.Error().empty(), "readonly target protected");
        NoTemporary(old);
        Check(!logger.SaveCopy((directory / L"missing" / L"output.log").wstring(), point, error, nullptr), "missing destination directory");
        evidence["cases"].push_back({{"name", "actual readonly replacement and missing-directory failures; target preserved"}});
        // The source is stopped, so truncation is controlled filesystem fault injection.
        fs::resize_file(second, 0);
        Check(!logger.SaveCopy(old.wstring(), point, error, nullptr) && Read(old) == original, "truncated source rejected");
        NoTemporary(old);
        evidence["cases"].push_back({{"name", "truncated source rejects incomplete snapshot; no old-target replacement"}});
        Check(logger.Start(L"new-generation", error, true), "restart logger");
        roots.push_back(logger.Path());
        Check(!logger.SaveCopy(old.wstring(), point, error, nullptr) && Read(old) == original, "stale generation rejected");
        const auto empty = directory / L"empty.csv";
        Check(logger.SaveCopy(empty.wstring(), error) && fs::file_size(empty) == 0, "empty CSV export");
        logger.WriteRaw(Bytes{'N', 'E', 'W'});
        const auto fresh = directory / L"fresh.log";
        Check(logger.SaveCopy(fresh.wstring(), error) && Read(fresh) == "NEW" && logger.Error().empty(), "logging/export recovers after destination failures");
        logger.Stop();
        evidence["cases"].push_back({{"name", "stale generation rejects; empty CSV and later fresh export succeed"}});
        evidence["success"] = true;
    } catch (const std::exception &error) {
        evidence["success"] = false;
        evidence["error"] = error.what();
        std::cerr << error.what() << '\n';
        exitCode = 1;
    }
    for (const auto &path : roots) { std::error_code ignored; fs::remove(path, ignored); }
    if (!directory.empty()) { std::error_code ignored; fs::remove_all(directory, ignored); }
    wchar_t evidenceDirectory[32768]{};
    if (const auto length = GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", evidenceDirectory, 32768)) {
        if (length < 32768) {
            std::ofstream file(fs::path(evidenceDirectory) / L"logger-export-native.json", std::ios::binary);
            file << evidence.dump(2);
        }
    }
    std::cout << evidence.dump() << '\n';
    return exitCode;
}
