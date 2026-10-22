#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace serialctl::update::testing {
using ZipBytes = std::vector<unsigned char>;
inline void Le(ZipBytes &bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        bytes.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
inline std::uint32_t ZipCrc(const ZipBytes &bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (auto byte : bytes) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc ^ 0xffffffffu;
}
inline ZipBytes RawDeflate(const ZipBytes &bytes) {
    // Independent stored-block encoder exercises ZIP method 8 without linking a
    // compressor into production. A known zlib dynamic-Huffman vector is below.
    if (bytes.size() == 100000 && std::all_of(bytes.begin(), bytes.end(), [](unsigned char c) { return c == 'X'; }))
        return ZipBytes{237, 193, 49, 1, 0, 0, 0, 194, 160, 154, 235, 159, 196, 26, 30, 64, 1, 0, 0, 0, 0, 0,   0,
                        0,   0,   0,  0, 0, 0, 0, 0,   0,   0,   0,   0,   0,   0,  0,  0,  0, 0, 0, 0, 0, 0,   0,
                        0,   0,   0,  0, 0, 0, 0, 0,   0,   0,   0,   0,   0,   0,  0,  0,  0, 0, 0, 0, 0, 0,   0,
                        0,   0,   0,  0, 0, 0, 0, 0,   0,   0,   0,   0,   0,   0,  0,  0,  0, 0, 0, 0, 0, 0,   0,
                        0,   0,   0,  0, 0, 0, 0, 0,   0,   0,   0,   0,   0,   0,  0,  0,  0, 0, 0, 0, 0, 175, 6};
    ZipBytes out;
    for (size_t pos = 0; pos < bytes.size();) {
        const auto length = static_cast<unsigned>(std::min<size_t>(65535, bytes.size() - pos));
        out.push_back(pos + length == bytes.size() ? 1 : 0);
        Le(out, length, 2);
        Le(out, length ^ 65535, 2);
        out.insert(out.end(), bytes.begin() + pos, bytes.begin() + pos + length);
        pos += length;
    }
    return out;
}
inline ZipBytes BuildTestZip(const std::map<std::string, ZipBytes> &files, bool deflated, bool wrongCrc = false) {
    ZipBytes out, directory;
    for (const auto &pair : files) {
        const auto compressed = deflated ? RawDeflate(pair.second) : pair.second;
        const auto crc = ZipCrc(pair.second) ^ (wrongCrc ? 1u : 0u);
        const auto offset = out.size();
        Le(out, 0x04034b50, 4);
        Le(out, 20, 2);
        Le(out, 0x0800, 2);
        Le(out, deflated ? 8 : 0, 2);
        Le(out, 0, 4);
        Le(out, crc, 4);
        Le(out, static_cast<std::uint32_t>(compressed.size()), 4);
        Le(out, static_cast<std::uint32_t>(pair.second.size()), 4);
        Le(out, static_cast<unsigned>(pair.first.size()), 2);
        Le(out, 0, 2);
        out.insert(out.end(), pair.first.begin(), pair.first.end());
        out.insert(out.end(), compressed.begin(), compressed.end());
        Le(directory, 0x02014b50, 4);
        Le(directory, 20, 2);
        Le(directory, 20, 2);
        Le(directory, 0x0800, 2);
        Le(directory, deflated ? 8 : 0, 2);
        Le(directory, 0, 4);
        Le(directory, crc, 4);
        Le(directory, static_cast<std::uint32_t>(compressed.size()), 4);
        Le(directory, static_cast<std::uint32_t>(pair.second.size()), 4);
        Le(directory, static_cast<unsigned>(pair.first.size()), 2);
        Le(directory, 0, 2);
        Le(directory, 0, 2);
        Le(directory, 0, 2);
        Le(directory, 0, 2);
        Le(directory, 0, 4);
        Le(directory, static_cast<std::uint32_t>(offset), 4);
        directory.insert(directory.end(), pair.first.begin(), pair.first.end());
    }
    const auto offset = out.size();
    out.insert(out.end(), directory.begin(), directory.end());
    Le(out, 0x06054b50, 4);
    Le(out, 0, 2);
    Le(out, 0, 2);
    Le(out, static_cast<unsigned>(files.size()), 2);
    Le(out, static_cast<unsigned>(files.size()), 2);
    Le(out, static_cast<std::uint32_t>(directory.size()), 4);
    Le(out, static_cast<std::uint32_t>(offset), 4);
    Le(out, 0, 2);
    return out;
}
} // namespace serialctl::update::testing
