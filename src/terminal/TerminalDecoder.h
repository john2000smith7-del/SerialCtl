#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#ifdef _WIN32
#include "TransferProtocol.h"
#include "Win32Helpers.h"
#include <array>
#endif
namespace serialctl {
// Walk from a known boundary: DBCS trail bytes may look like lead bytes.
template <class IsLeadByte> size_t IncompleteDbcsTail(const std::vector<std::uint8_t> &bytes, IsLeadByte isLead) {
    size_t index = 0;
    while (index < bytes.size()) {
        if (isLead(bytes[index])) {
            if (index + 1 == bytes.size())
                return 1;
            index += 2;
        } else
            ++index;
    }
    return 0;
}
inline bool IsValidUtf8TrailByte(std::uint8_t lead, size_t j, std::uint8_t b) {
    if ((b & 0xc0) != 0x80) return false;
    if (j == 1) {
        if (lead == 0xe0 && b < 0xa0) return false; // Overlong 3-byte
        if (lead == 0xed && b > 0x9f) return false; // Surrogate pair U+D800..U+DFFF
        if (lead == 0xf0 && b < 0x90) return false; // Overlong 4-byte
        if (lead == 0xf4 && b > 0x8f) return false; // Above U+10FFFF
    }
    return true;
}

inline size_t IncompleteUtf8Tail(const std::vector<std::uint8_t> &bytes) {
    size_t index = 0;
    while (index < bytes.size()) {
        auto lead = bytes[index];
        size_t length = lead >= 0xc2 && lead <= 0xdf   ? 2
                        : lead >= 0xe0 && lead <= 0xef ? 3
                        : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                       : 1;
        size_t available = bytes.size() - index;
        bool valid = true;
        for (size_t j = 1; j < length && j < available; ++j)
            valid = valid && IsValidUtf8TrailByte(lead, j, bytes[index + j]);
        if (valid && available < length)
            return available;
        index += valid ? length : 1;
    }
    return 0;
}
#ifdef _WIN32
class TerminalDecoder {
  public:
    std::wstring Decode(const std::vector<std::uint8_t> &data, UINT page) {
        std::wstring result;
        if (page_ && page_ != page && !pending_.empty()) {
            result = L"\xfffd";
            pending_.clear();
            zmodemScanState_ = ZmodemScan::Idle;
            zmodemHexCount_ = 0;
        }
        page_ = page;
        size_t start = pending_.size();
        pending_.insert(pending_.end(), data.begin(), data.end());

        // ZMODEM HEX header trailing control byte normalization:
        // When detecting a ZMODEM HEX frame (**\x18B followed by hex digits),
        // normalize frame ending sequence (\r\x8a or \x8d\x8a) so 0x8a -> 0x0a ('\n')
        // and 0x8d -> 0x0d ('\r') without producing U+FFFD question marks.
        for (size_t i = start; i < pending_.size(); ++i) {
            std::uint8_t b = pending_[i];
            switch (zmodemScanState_) {
            case ZmodemScan::Idle:
                if (b == '*') zmodemScanState_ = ZmodemScan::Star1;
                break;
            case ZmodemScan::Star1:
                if (b == '*') zmodemScanState_ = ZmodemScan::Star2;
                else zmodemScanState_ = ZmodemScan::Idle;
                break;
            case ZmodemScan::Star2:
                if (b == 0x18) zmodemScanState_ = ZmodemScan::Zdle;
                else if (b == '*') zmodemScanState_ = ZmodemScan::Star2;
                else zmodemScanState_ = ZmodemScan::Idle;
                break;
            case ZmodemScan::Zdle:
                if (b == 'B') {
                    zmodemScanState_ = ZmodemScan::Hex;
                    zmodemHexCount_ = 0;
                } else {
                    zmodemScanState_ = (b == '*') ? ZmodemScan::Star1 : ZmodemScan::Idle;
                }
                break;
            case ZmodemScan::Hex:
                if (const int digit = HexDigit(b); digit >= 0 && zmodemHexCount_ < 2 * zmodemHeader_.size()) {
                    auto &byte = zmodemHeader_[zmodemHexCount_ / 2];
                    byte = static_cast<std::uint8_t>(zmodemHexCount_ % 2 ? byte | digit : digit << 4);
                    ++zmodemHexCount_;
                } else if (zmodemHexCount_ == 2 * zmodemHeader_.size() && (b == 0x0d || b == 0x8d) &&
                           xfer::Crc16(zmodemHeader_.data(), zmodemHeader_.size()) == 0) {
                    if (b == 0x8d) pending_[i] = 0x0d;
                    zmodemScanState_ = ZmodemScan::SawCr;
                } else {
                    zmodemScanState_ = (b == '*') ? ZmodemScan::Star1 : ZmodemScan::Idle;
                }
                break;
            case ZmodemScan::SawCr:
                if (b == 0x8a) {
                    pending_[i] = 0x0a;
                    zmodemScanState_ = ZmodemScan::Idle;
                } else if (b == 0x0d || b == 0x8d) {
                    if (b == 0x8d) pending_[i] = 0x0d;
                } else if (b == 0x0a) {
                    zmodemScanState_ = ZmodemScan::Idle;
                } else {
                    zmodemScanState_ = (b == '*') ? ZmodemScan::Star1 : ZmodemScan::Idle;
                }
                break;
            }
        }

        UINT effective = page == 20936 && !IsValidCodePage(20936) ? 936 : page;
        size_t tail = effective == CP_UTF8 ? IncompleteUtf8Tail(pending_)
                                           : IncompleteDbcsTail(pending_, [effective](std::uint8_t b) {
                                                 return IsDBCSLeadByteEx(effective, b) != FALSE;
                                             });
        size_t n = pending_.size() - tail;
        result += MultiByteToWide(pending_.data(), n, effective);
        pending_.erase(pending_.begin(), pending_.begin() + n);
        return result;
    }
    void Reset() {
        pending_.clear();
        page_ = 0;
        zmodemScanState_ = ZmodemScan::Idle;
        zmodemHexCount_ = 0;
    }

  private:
    static int HexDigit(std::uint8_t byte) {
        if (byte >= '0' && byte <= '9') return byte - '0';
        if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
        if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
        return -1;
    }
    enum class ZmodemScan { Idle, Star1, Star2, Zdle, Hex, SawCr };
    ZmodemScan zmodemScanState_ = ZmodemScan::Idle;
    // ZHEX has five header bytes followed by a big-endian CRC-16.
    std::array<std::uint8_t, 7> zmodemHeader_{};
    size_t zmodemHexCount_ = 0;
    std::vector<std::uint8_t> pending_;
    UINT page_ = 0;
};
#endif
} // namespace serialctl
