#include "UpdateManifest.h"
#include "UpdateTrustKey.h"
#include "json.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>

namespace serialctl::update {
namespace {

struct Provider {
    HCRYPTPROV value = 0;
    Provider() {
        if (!CryptAcquireContextW(&value, nullptr, MS_ENH_RSA_AES_PROV_W,
                                  PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
            throw std::runtime_error("Cannot initialize SHA-256 provider");
    }
    ~Provider() { if (value) CryptReleaseContext(value, 0); }
};
struct Hash {
    HCRYPTHASH value = 0;
    explicit Hash(HCRYPTPROV provider) {
        if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &value))
            throw std::runtime_error("Cannot initialize SHA-256 hash");
    }
    ~Hash() { if (value) CryptDestroyHash(value); }
    void Add(const unsigned char* data, DWORD size) {
        if (!CryptHashData(value, data, size, 0)) throw std::runtime_error("SHA-256 failed");
    }
};
struct Key {
    HCRYPTKEY value = 0;
    ~Key() { if (value) CryptDestroyKey(value); }
};
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit File(const std::filesystem::path& path) {
        value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot read update file");
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(value, &information) ||
            (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            CloseHandle(value);
            value = INVALID_HANDLE_VALUE;
            throw std::runtime_error("Update file must be a regular file");
        }
    }
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

void CheckPath(const std::filesystem::path& path) {
    std::filesystem::path current;
    for (const auto& part : std::filesystem::absolute(path).lexically_normal()) {
        current /= part;
        // A bare drive name such as C: is not yet an absolute path.
        if (current == current.root_name()) continue;
        DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Missing update path or reparse point");
    }
}

std::vector<unsigned char> Read(const std::filesystem::path& path, std::uint64_t maximum) {
    CheckPath(path);
    File file(path);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > maximum)
        throw std::runtime_error("Update file exceeds its size limit");
    std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD count = 0;
    if (!bytes.empty() && (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
                          count != bytes.size()))
        throw std::runtime_error("Update file is truncated");
    return bytes;
}

std::array<unsigned int, 3> Version(const std::string& text) {
    std::array<unsigned int, 3> result{};
    if (text.size() < 6 || text.size() > 19 || text[0] != 'V')
        throw std::runtime_error("Invalid version; expected canonical Vx.y.z");
    size_t position = 1;
    for (size_t part = 0; part < result.size(); ++part) {
        size_t start = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            result[part] = result[part] * 10 + static_cast<unsigned int>(text[position++] - '0');
            if (result[part] > 65535) throw std::runtime_error("Version component exceeds 65535");
        }
        if (position == start || (position - start > 1 && text[start] == '0'))
            throw std::runtime_error("Invalid version component");
        if (part != 2) {
            if (position == text.size() || text[position++] != '.') throw std::runtime_error("Invalid version");
        } else if (position != text.size()) throw std::runtime_error("Invalid version suffix");
    }
    return result;
}

bool SafeRelativeUrl(const std::string& url) {
    if (url.empty() || url.size() > 512 || url.front() == '/' || url.back() == '/') return false;
    for (unsigned char c : url)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == '/')) return false;
    size_t start = 0;
    while (start < url.size()) {
        size_t end = url.find('/', start);
        const std::string part = url.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

Manifest Verify(const std::vector<unsigned char>& bytes, const std::vector<unsigned char>& signature,
                const unsigned char* publicKey, size_t keySize) {
    if (bytes.empty() || bytes.size() > kMaxManifestBytes || signature.size() != 256 || keySize > 4096)
        throw std::runtime_error("Invalid manifest or signature size");
    Provider provider;
    Key key;
    if (!CryptImportKey(provider.value, publicKey, static_cast<DWORD>(keySize), 0, 0, &key.value))
        throw std::runtime_error("Cannot import update trust key");
    Hash hash(provider.value);
    hash.Add(bytes.data(), static_cast<DWORD>(bytes.size()));
    // CryptoAPI expects little-endian RSA integers; wire format follows OpenSSL.
    std::vector<unsigned char> littleEndian(signature.rbegin(), signature.rend());
    if (!CryptVerifySignatureW(hash.value, littleEndian.data(), static_cast<DWORD>(littleEndian.size()),
                               key.value, nullptr, 0))
        throw std::runtime_error("Update manifest signature is invalid");

    using Json = nlohmann::json;
    // Reject deeply nested/duplicate-key documents, even when signed by mistake.
    std::vector<std::set<std::string>> objectKeys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 8) throw std::runtime_error("Manifest nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) objectKeys.emplace_back();
        if (event == Json::parse_event_t::key &&
            !objectKeys.back().insert(parsed.get<std::string>()).second)
            throw std::runtime_error("Duplicate manifest key");
        if (event == Json::parse_event_t::object_end) objectKeys.pop_back();
        return true;
    };
    const Json json = Json::parse(bytes.begin(), bytes.end(), callback);
    if (!json.is_object() || json.size() != 5 || !json.contains("schema") ||
        !json["schema"].is_number_integer() || json["schema"] != 1 ||
        json.value("product", Json()) != "SerialCtl" || json.value("architecture", Json()) != "x64" ||
        !json.contains("version") || !json["version"].is_string() ||
        !json.contains("files") || !json["files"].is_array() ||
        json["files"].empty() || json["files"].size() > kMaxUpdateFiles)
        throw std::runtime_error("Unsupported update manifest schema, product or architecture");
    Manifest manifest;
    manifest.version = json["version"].get<std::string>();
    Version(manifest.version);
    std::uint64_t total = 0;
    std::set<std::string> names;
    for (const auto& item : json["files"]) {
        if (!item.is_object() || item.size() != 4 || !item.contains("name") || !item["name"].is_string() ||
            !item.contains("size") || !item["size"].is_number_unsigned() ||
            !item.contains("sha256") || !item["sha256"].is_string() ||
            !item.contains("url") || !item["url"].is_string())
            throw std::runtime_error("Invalid update file record");
        UpdateFile file{item["name"].get<std::string>(), item["size"].get<std::uint64_t>(),
                        item["sha256"].get<std::string>(), item["url"].get<std::string>()};
        if (!IsAllowedUpdateFile(file.name) || !names.insert(file.name).second ||
            file.size == 0 || file.size > kMaxUpdateFileBytes ||
            file.sha256.size() != 64 || !std::all_of(file.sha256.begin(), file.sha256.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }) || !SafeRelativeUrl(file.url))
            throw std::runtime_error("Unsafe update filename, URL, size or checksum");
        total += file.size;
        if (total > kMaxUpdateTotalBytes) throw std::runtime_error("Update total size exceeds limit");
        manifest.files.push_back(std::move(file));
    }
    for (const char* required : {"serialctl.exe", "plink.exe", "psftp.exe", "VERSION"})
        if (!names.count(required)) throw std::runtime_error("Update is missing a required runtime file");
    return manifest;
}

Manifest VerifyStage(const std::filesystem::path& stage, const std::string& currentVersion,
                     const unsigned char* key, size_t keySize, const std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Update cancelled");
    CheckPath(stage);
    const auto manifest = Verify(Read(stage / L"update.json", kMaxManifestBytes),
                                 Read(stage / L"update.json.sig", 256), key, keySize);
    if (CompareVersions(manifest.version, currentVersion) <= 0)
        throw std::runtime_error("Staged version is not newer than the running version");
    std::set<std::filesystem::path> expected;
    for (const auto& file : manifest.files) {
        if (cancel && cancel->load()) throw std::runtime_error("Update cancelled");
        const auto name = std::filesystem::u8path(file.name);
        const auto path = stage / L"files" / name;
        CheckPath(path);
        if (std::filesystem::file_size(path) != file.size || Sha256File(path, cancel) != file.sha256)
            throw std::runtime_error("Staged file size or SHA-256 mismatch");
        expected.insert(name);
    }
    for (const auto& entry : std::filesystem::directory_iterator(stage / L"files"))
        if (!expected.erase(entry.path().filename())) throw std::runtime_error("Unexpected staged payload file");
    if (!expected.empty()) throw std::runtime_error("Missing staged payload file");
    const auto versionBytes = Read(stage / L"files" / L"VERSION", 256);
    std::string versionText(versionBytes.begin(), versionBytes.end());
    versionText.erase(std::remove(versionText.begin(), versionText.end(), '\r'), versionText.end());
    if (!versionText.empty() && versionText.back() == '\n') versionText.pop_back();
    if (versionText != "PROJECT_VERSION=" + manifest.version + "\nCPP_VERSION=" + manifest.version)
        throw std::runtime_error("VERSION does not match the signed version");
    return manifest;
}

} // namespace

int CompareVersions(const std::string& left, const std::string& right) {
    const auto a = Version(left), b = Version(right);
    return a < b ? -1 : a == b ? 0 : 1;
}

bool IsAllowedUpdateFile(const std::string& name) {
    static const std::set<std::string> allowed = {
        "serialctl.exe", "plink.exe", "psftp.exe", "VERSION", "README.md", "RELEASE-NOTES.md", "AUTO-UPDATE.md",
        "AI-API.md", "AI-SERIAL.md", "WEBSOCKET-PROTOCOL.md", "POWER.md", "THIRD-PARTY-NOTICES.md",
        "JSON-LICENSE.txt", "PUTTY-LICENCE.txt", "YY-THUNKS-LICENSE.txt", "WIN7-HARDWARE-TEST.md",
        "MANUAL-REGRESSION.md", "V1.0.6-ACCEPTANCE.md", "AUTO-UPDATE-ACCEPTANCE.md",
        "serialctl_client.py", "serialctl_api.py", "serialctl_ws.py", u8"常用命令示例.txt"};
    return allowed.count(name) != 0;
}

std::string Sha256File(const std::filesystem::path& path, const std::atomic<bool>* cancel) {
    CheckPath(path);
    File file(path);
    Provider provider;
    Hash hash(provider.value);
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        if (cancel && cancel->load()) throw std::runtime_error("Update cancelled");
        DWORD count = 0;
        if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            throw std::runtime_error("Cannot hash update file");
        if (!count) break;
        hash.Add(buffer.data(), count);
    }
    std::array<unsigned char, 32> digest{};
    DWORD size = static_cast<DWORD>(digest.size());
    if (!CryptGetHashParam(hash.value, HP_HASHVAL, digest.data(), &size, 0))
        throw std::runtime_error("Cannot finish SHA-256 hash");
    std::string result;
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

Manifest VerifySignedManifest(const std::vector<unsigned char>& data,
                              const std::vector<unsigned char>& signature) {
    return Verify(data, signature, kTrustedPublicKey, sizeof(kTrustedPublicKey));
}
Manifest VerifyStagedManifest(const std::filesystem::path& stage, const std::string& currentVersion,
                              const std::atomic<bool>* cancel) {
    return VerifyStage(stage, currentVersion, kTrustedPublicKey, sizeof(kTrustedPublicKey), cancel);
}

#ifdef SERIALCTL_UPDATE_TESTING
namespace testing {
Manifest VerifySignedManifest(const std::vector<unsigned char>& data, const std::vector<unsigned char>& signature,
                              const std::vector<unsigned char>& key) {
    return Verify(data, signature, key.data(), key.size());
}
Manifest VerifyStagedManifest(const std::filesystem::path& stage, const std::string& currentVersion,
                             const std::vector<unsigned char>& key, const std::atomic<bool>* cancel) {
    return VerifyStage(stage, currentVersion, key.data(), key.size(), cancel);
}
}
#endif

} // namespace serialctl::update
