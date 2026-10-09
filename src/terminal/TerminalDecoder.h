#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace serialctl {
// Walk from a known character boundary: a DBCS trail byte may also look
// like a lead byte, so examining only the final byte is incorrect.
template<class IsLeadByte>
size_t IncompleteDbcsTail(const std::vector<std::uint8_t>& bytes, IsLeadByte isLead) {
    size_t index = 0;
    while (index < bytes.size()) {
        if (isLead(bytes[index])) {
            if (index + 1 == bytes.size()) return 1;
            index += 2;
        } else {
            ++index;
        }
    }
    return 0;
}
} // namespace serialctl
