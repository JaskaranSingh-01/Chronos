#pragma once

#include <cstdint>

namespace simulator {

namespace itch {

inline constexpr std::uint8_t ADD_ORDER = 0x41;       // 'A'
inline constexpr std::uint8_t ORDER_EXECUTED = 0x45;  // 'E'
inline constexpr std::uint8_t ORDER_CANCEL = 0x58;    // 'X'
inline constexpr std::uint8_t ORDER_DELETE = 0x44;    // 'D'
inline constexpr std::uint8_t REPLACE_ORDER = 0x55;   // 'U'

} // namespace itch

} // namespace simulator