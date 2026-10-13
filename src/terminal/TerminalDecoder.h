#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#ifdef _WIN32
#include "Win32Helpers.h"
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
            valid = valid && ((bytes[index + j] & 0xc0) == 0x80);
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
        }
        page_ = page;
        pending_.insert(pending_.end(), data.begin(), data.end());
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
    }

  private:
    std::vector<std::uint8_t> pending_;
    UINT page_ = 0;
};
#endif
} // namespace serialctl
