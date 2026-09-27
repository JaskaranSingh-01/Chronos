#pragma once

#include <compare>
#include <cstdint>

namespace simulator
{
    struct Price
    {
        std::int64_t ticks{0};

        constexpr bool operator==(const Price&) const = default;

        constexpr auto operator<=>(const Price&) const = default;

    };
    
} // namespace simulator
