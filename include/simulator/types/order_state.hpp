#pragma once

#include <cstdint>

namespace simulator {

enum class OrderState : std::uint8_t {
    New,
    Active,
    PartiallyFilled,
    Filled,
    Cancelled,
    Deleted
};

} // namespace simulator