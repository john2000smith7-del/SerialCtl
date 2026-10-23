#pragma once

#include "UpdateManifest.h"
#include <functional>

namespace serialctl::update {

enum class UpdateStatus { Disabled, NoUpdate, Ready, Failed, Cancelled };

enum class UpdatePhase { Download, Verify };
struct UpdateCallbacks {
    // Called only after authenticity, newer-version and quarantine checks. No ZIP
    // or staging directory exists until this returns true. Never on UI thread.
    std::function<bool(const std::string &)> confirm;
    std::function<void(std::uint64_t, std::uint64_t, UpdatePhase)> progress;
};
struct UpdateResult {
    UpdateStatus status = UpdateStatus::Failed;
    std::wstring message;
    std::filesystem::path stagingDirectory;
    std::string version;
};

// Blocking worker API. Never invoke on the UI thread. Uses exe-adjacent update.ini,
// [update] url=<absolute HTTP(S) manifest URL>. Empty/missing means no network access.
// No execution or replacement occurs here; a Ready result owns its staging directory.
UpdateResult CheckAndStage(const std::filesystem::path &exePath, const std::string &currentVersion,
                           const std::atomic<bool> &cancel, const UpdateCallbacks &callbacks = {});

#ifdef SERIALCTL_UPDATE_TESTING
namespace testing {
struct HttpLifetimeCounts {
    unsigned long long created = 0;
    unsigned long long destroyed = 0;
    unsigned long long closingCallbacks = 0;
};
HttpLifetimeCounts GetHttpLifetimeCounts();
UpdateResult CheckAndStage(const std::filesystem::path &exePath, const std::string &currentVersion,
                           const std::atomic<bool> &cancel, const std::vector<unsigned char> &publicKey,
                           const UpdateCallbacks &callbacks = {});
}
#endif

} // namespace serialctl::update
