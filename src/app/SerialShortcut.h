#pragma once
#include <array>
#include <cstdint>
namespace serialctl {
inline constexpr std::array<std::uint8_t, 8> SerialF2Payload() {
    return {{'2', '3', '2', 's', 't', 'a', 'r', 't'}};
}
inline constexpr std::uint32_t FlipRegisterBit(std::uint32_t value, unsigned bit) {
    return bit < 32 ? value ^ (std::uint32_t{1} << bit) : value;
}
} // namespace serialctl
