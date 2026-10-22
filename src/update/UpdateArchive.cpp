#include "../../third_party/miniz/miniz.h"
#include "UpdateManifest.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <windows.h>

namespace serialctl::update {
namespace {
using Bytes = std::vector<unsigned char>;
void Cancel(const std::atomic<bool> &cancel) {
    if (cancel.load())
        throw std::runtime_error("Update cancelled");
}
unsigned U16(const Bytes &bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2)
        throw std::runtime_error("Truncated ZIP header");
    return bytes[offset] | (unsigned(bytes[offset + 1]) << 8);
}
std::uint32_t U32(const Bytes &bytes, size_t offset) {
    return U16(bytes, offset) | (std::uint32_t(U16(bytes, offset + 2)) << 16);
}
Bytes Read(std::ifstream &file, std::uint64_t offset, size_t size, const std::atomic<bool> &cancel) {
    file.seekg(static_cast<std::streamoff>(offset));
    Bytes bytes(size);
    for (size_t done = 0; done < size;) {
        Cancel(cancel);
        const size_t count = std::min<size_t>(65536, size - done);
        if (!file.read(reinterpret_cast<char *>(bytes.data() + done), static_cast<std::streamsize>(count)))
            throw std::runtime_error("Truncated ZIP data");
        done += count;
    }
    return bytes;
}
std::string Name(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.size() > 1024 || name.front() == '/')
        throw std::runtime_error("Unsafe ZIP path");
    for (unsigned char c : name)
        if (c < 32 || c == 127 || c == ':')
            throw std::runtime_error("Unsafe ZIP filename");
    std::string check = name;
    if (check.back() == '/')
        check.pop_back();
    size_t start = 0;
    while (start < check.size()) {
        auto end = check.find('/', start);
        const auto part = check.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            throw std::runtime_error("Unsafe ZIP path component");
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (check.empty())
        throw std::runtime_error("Empty ZIP path");
    return name;
}
std::uint32_t Crc(const Bytes &bytes, const std::atomic<bool> &cancel) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (unsigned i = 0; i < result.size(); ++i) {
            auto c = i;
            for (int bit = 0; bit < 8; ++bit)
                c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
            result[i] = c;
        }
        return result;
    }();
    std::uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (!(i % 65536))
            Cancel(cancel);
        crc = table[(crc ^ bytes[i]) & 255] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}
void Save(const std::filesystem::path &path, const Bytes &bytes, const std::atomic<bool> &cancel) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create extracted update file");
    struct Close {
        HANDLE file;
        ~Close() {
            CloseHandle(file);
        }
    } close{file};
    for (size_t done = 0; done < bytes.size();) {
        Cancel(cancel);
        const auto count = static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - done));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + done, count, &written, nullptr) || written != count)
            throw std::runtime_error("Cannot write extracted update file");
        done += count;
    }
    if (!FlushFileBuffers(file))
        throw std::runtime_error("Cannot flush extracted update file");
}
struct Entry {
    std::string rawName;
    unsigned flags, method;
    std::uint32_t crc, compressed, size, offset;
};
} // namespace

void ExtractUpdateArchive(const std::filesystem::path &archive, const Manifest &manifest,
                          const std::filesystem::path &destination, const std::atomic<bool> &cancel) {
    Cancel(cancel);
    if (!manifest.archive.size || manifest.archive.size > kMaxArchiveBytes ||
        std::filesystem::file_size(archive) != manifest.archive.size ||
        Sha256File(archive, &cancel) != manifest.archive.sha256)
        throw std::runtime_error("ZIP size or SHA-256 differs from signed metadata");
    std::filesystem::path current;
    for (const auto &part : std::filesystem::absolute(destination).lexically_normal()) {
        current /= part;
        if (current == current.root_name())
            continue;
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Unsafe ZIP output directory");
    }
    std::ifstream file(archive, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open update ZIP");
    const auto length = manifest.archive.size;
    const size_t tailSize = static_cast<size_t>(std::min<std::uint64_t>(length, 65557));
    auto tail = Read(file, length - tailSize, tailSize, cancel);
    size_t end = tail.size();
    while (end >= 22) {
        --end;
        if (end + 22 <= tail.size() && U32(tail, end) == 0x06054b50u && end + 22 + U16(tail, end + 20) == tail.size())
            break;
    }
    if (end + 22 > tail.size() || U32(tail, end) != 0x06054b50u || U16(tail, end + 4) || U16(tail, end + 6))
        throw std::runtime_error("ZIP central directory missing, ZIP64 or split archive unsupported");
    const auto count = U16(tail, end + 10);
    const auto centralSize = U32(tail, end + 12), centralOffset = U32(tail, end + 16);
    if (!count || count > 4096 || count != U16(tail, end + 8) || centralSize > 8 * 1024 * 1024 ||
        std::uint64_t(centralOffset) + centralSize != length - tailSize + end)
        throw std::runtime_error("Invalid ZIP directory bounds or entry count");
    auto central = Read(file, centralOffset, centralSize, cancel);
    std::map<std::string, Entry> entries;
    size_t pos = 0;
    for (unsigned index = 0; index < count; ++index) {
        Cancel(cancel);
        if (central.size() - pos < 46 || U32(central, pos) != 0x02014b50u)
            throw std::runtime_error("Invalid ZIP central record");
        const auto nameLength = U16(central, pos + 28), extra = U16(central, pos + 30),
                   comment = U16(central, pos + 32);
        const auto next = pos + 46 + nameLength + extra + comment;
        if (next > central.size() || U16(central, pos + 34))
            throw std::runtime_error("Truncated or split ZIP entry");
        Entry entry{std::string(reinterpret_cast<const char *>(central.data() + pos + 46), nameLength),
                    U16(central, pos + 8),
                    U16(central, pos + 10),
                    U32(central, pos + 16),
                    U32(central, pos + 20),
                    U32(central, pos + 24),
                    U32(central, pos + 42)};
        const auto attributes = U32(central, pos + 38);
        if ((entry.flags & ~0x080eu) || (entry.method != 0 && entry.method != 8) ||
            ((attributes >> 16) & 0170000u) == 0120000u || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            entry.offset >= centralOffset || !entries.emplace(Name(entry.rawName), entry).second)
            throw std::runtime_error("Encrypted, duplicate, symlink or unsupported ZIP entry");
        pos = next;
    }
    if (pos != central.size())
        throw std::runtime_error("Unexpected ZIP directory trailing data");
    // Only signed, flat runtime names are ever written. Driver/config/other
    // release members remain inside the archive, regardless of their contents.
    std::set<std::string> written;
    std::uint64_t total = 0;
    for (const auto &expected : manifest.files) {
        Cancel(cancel);
        if (!IsAllowedUpdateFile(expected.name) || !written.insert(expected.name).second || !expected.size ||
            expected.size > kMaxUpdateFileBytes || (total += expected.size) > kMaxUpdateTotalBytes)
            throw std::runtime_error("Unsafe extracted update payload");
        const auto found = entries.find(expected.name);
        if (found == entries.end())
            throw std::runtime_error("Required runtime missing from ZIP");
        const auto &entry = found->second;
        if (entry.size != expected.size || entry.compressed > kMaxUpdateFileBytes + 1024 * 1024)
            throw std::runtime_error("ZIP entry size differs from signed metadata");
        auto local = Read(file, entry.offset, 30, cancel);
        const auto nameLength = U16(local, 26), extra = U16(local, 28);
        const auto dataOffset = std::uint64_t(entry.offset) + 30 + nameLength + extra;
        if (U32(local, 0) != 0x04034b50u || U16(local, 6) != entry.flags || U16(local, 8) != entry.method ||
            dataOffset + entry.compressed > centralOffset)
            throw std::runtime_error("ZIP local header differs from directory");
        auto rawName = Read(file, entry.offset + 30, nameLength, cancel);
        if (std::string(rawName.begin(), rawName.end()) != entry.rawName ||
            (!(entry.flags & 8) &&
             (U32(local, 14) != entry.crc || U32(local, 18) != entry.compressed || U32(local, 22) != entry.size)))
            throw std::runtime_error("ZIP local name, CRC or lengths differ");
        auto compressed = Read(file, dataOffset, entry.compressed, cancel);
        Bytes output(static_cast<size_t>(expected.size));
        if (entry.method == 0) {
            if (compressed.size() != output.size())
                throw std::runtime_error("Invalid stored ZIP length");
            output = std::move(compressed);
        } else {
            tinfl_decompressor inflater{};
            tinfl_init(&inflater);
            if (compressed.empty()) throw std::runtime_error("Empty DEFLATE input");
            size_t consumed = 0, produced = 0;
            for (;;) {
                Cancel(cancel);
                size_t inputSize = std::min<size_t>(65536, compressed.size() - consumed);
                size_t outputSize = std::min<size_t>(65536, output.size() - produced);
                const auto flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                    (inputSize < compressed.size() - consumed ? TINFL_FLAG_HAS_MORE_INPUT : 0);
                const auto status = tinfl_decompress(&inflater, compressed.data() + consumed, &inputSize,
                    output.data(), output.data() + produced, &outputSize, flags);
                consumed += inputSize;
                produced += outputSize;
                if (status == TINFL_STATUS_DONE) {
                    if (consumed != compressed.size() || produced != output.size())
                        throw std::runtime_error("DEFLATE lengths differ from ZIP metadata");
                    break;
                }
                if (status < 0 || (!inputSize && !outputSize) ||
                    (status == TINFL_STATUS_NEEDS_MORE_INPUT && consumed == compressed.size()))
                    throw std::runtime_error("Deflated ZIP entry is corrupt or exceeds its output bound");
            }
        }
        Cancel(cancel);
        if (Crc(output, cancel) != entry.crc)
            throw std::runtime_error("ZIP CRC mismatch");
        const auto path = destination / std::filesystem::u8path(expected.name);
        Save(path, output, cancel);
        if (Sha256File(path, &cancel) != expected.sha256)
            throw std::runtime_error("Extracted runtime SHA-256 mismatch");
    }
}
} // namespace serialctl::update
