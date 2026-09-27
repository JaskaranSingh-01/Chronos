#pragma once

#include <cstdint>

namespace simulator
{
    struct Quantity
    {
        std::uint32_t value{0};

        constexpr bool operator==(const Quantity&) const = default;

        constexpr auto operator<=>(const Quantity&) const = default;

    };
    
} // namespace simulator
