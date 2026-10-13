#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace serialctl::update {

constexpr std::uint64_t kMaxUpdateFileBytes = 32ull * 1024 * 1024;
constexpr std::uint64_t kMaxUpdateTotalBytes = 64ull * 1024 * 1024;
constexpr std::size_t kMaxManifestBytes = 64 * 1024;
constexpr std::size_t kMaxUpdateFiles = 32;

struct UpdateFile {
    std::string name; // exact, case-sensitive member of the flat runtime allowlist
    std::uint64_t size = 0;
    std::string sha256;
    std::string url; // relative URL, same origin as the signed manifest
};

struct Manifest {
    std::string version;
    std::vector<UpdateFile> files;
};

// Returns -1/0/1; throws std::runtime_error for anything except canonical Vx.y.z.
int CompareVersions(const std::string& left, const std::string& right);
bool IsAllowedUpdateFile(const std::string& name);

// All failures throw std::runtime_error. Signature verification precedes JSON parsing.
Manifest VerifySignedManifest(const std::vector<unsigned char>& data,
                              const std::vector<unsigned char>& signature);
std::string Sha256File(const std::filesystem::path& path,
                       const std::atomic<bool>* cancel = nullptr);
// Re-verifies the signed manifest and every file immediately before installation.
// VERSION contents must agree with the signed version. Reparse points are rejected.
Manifest VerifyStagedManifest(const std::filesystem::path& stage,
                              const std::string& currentVersion,
                              const std::atomic<bool>* cancel = nullptr);

#ifdef SERIALCTL_UPDATE_TESTING
namespace testing {
Manifest VerifySignedManifest(const std::vector<unsigned char>& data,
                              const std::vector<unsigned char>& signature,
                              const std::vector<unsigned char>& publicKey);
Manifest VerifyStagedManifest(const std::filesystem::path& stage,
                             const std::string& currentVersion,
                             const std::vector<unsigned char>& publicKey,
                             const std::atomic<bool>* cancel = nullptr);
}
#endif

} // namespace serialctl::update
