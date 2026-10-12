#pragma once

#include "UpdateManifest.h"

namespace serialctl::update {

enum class UpdateStatus { Disabled, NoUpdate, Ready, Failed, Cancelled };

struct UpdateResult {
    UpdateStatus status = UpdateStatus::Failed;
    std::wstring message;
    std::filesystem::path stagingDirectory;
    std::string version;
};

// Blocking worker API. Never invoke on the UI thread. Uses exe-adjacent update.ini,
// [update] url=<absolute HTTP(S) manifest URL>. Empty/missing means no network access.
// No execution or replacement occurs here; a Ready result owns its staging directory.
UpdateResult CheckAndStage(const std::filesystem::path& exePath,
                           const std::string& currentVersion,
                           const std::atomic<bool>& cancel);

#ifdef SERIALCTL_UPDATE_TESTING
namespace testing {
UpdateResult CheckAndStage(const std::filesystem::path& exePath,
                          const std::string& currentVersion,
                          const std::atomic<bool>& cancel,
                          const std::vector<unsigned char>& publicKey);
}
#endif

} // namespace serialctl::update
