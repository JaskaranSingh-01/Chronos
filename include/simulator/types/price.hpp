#pragma once

#include <compare>
#include <cstdint>

namespace simulator
{
    // Fixed-point price: value = ticks x 10^-price_scale of the instrument.
    struct Price
    {
        std::int64_t ticks{0};

        constexpr bool operator==(const Price&) const = default;

        constexpr auto operator<=>(const Price&) const = default;

    };
    
} // namespace simulator
