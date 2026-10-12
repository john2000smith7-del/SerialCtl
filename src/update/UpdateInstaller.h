#pragma once

#include <filesystem>
#include <string>
#include <windows.h>

namespace serialctl::update {

enum class StartupAction { Continue, Exit, Failed };

// Call before initializing the GUI, sockets, or device services. Returns true only
// for the internal worker mode, including invalid worker invocations.
bool HandleUpdateCommandLine(const std::wstring& parameters, int& exitCode);

// Holds the per-installation startup/update lock until the main window exists.
// An interrupted transaction starts a recovery worker and returns Exit. Never
// start device services after Exit or Failed. Confirm is the health handshake.
class StartupGuard {
public:
    StartupGuard() = default;
    ~StartupGuard();
    StartupGuard(const StartupGuard&) = delete;
    StartupGuard& operator=(const StartupGuard&) = delete;
    StartupAction Begin(const std::wstring& parameters, std::wstring& error);
    bool Confirm(std::wstring& error);
    bool ShouldCheckForUpdate() const { return !recovered_; }

private:
    bool recovered_ = false;
    HANDLE instance_ = nullptr;
    HANDLE mutex_ = nullptr;
    HANDLE acknowledgment_ = nullptr;
    HANDLE committed_ = nullptr;
    HANDLE worker_ = nullptr;
};

// Call only after the GUI proves that there are no live connections, child
// processes, transfers, or power tasks. Success means the verified worker has
// acquired the lock; the caller must then exit normally. It never kills callers.
bool LaunchUpdateWorker(const std::filesystem::path& stagingDirectory,
                        const std::string& currentVersion,
                        std::wstring& error);

#ifdef SERIALCTL_UPDATE_TESTING
namespace testing {
// Controlled filesystem fixture: substitutes authenticated payloads only. Uses
// the same backup, durable journal, Windows replacement and recovery code.
void ApplyTransaction(const std::filesystem::path& installation,
                      const std::filesystem::path& stage, int failAfter = -1);
void RecoverTransaction(const std::filesystem::path& installation);
// Full worker/process handshake with an ephemeral test trust key stored only in
// the fixture stage. Production binaries do not contain this entry point.
bool LaunchSignedTestUpdate(const std::filesystem::path& stage,
                            const std::string& currentVersion, std::wstring& error);
}
#endif

} // namespace serialctl::update
