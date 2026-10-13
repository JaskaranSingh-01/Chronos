#pragma once

#include <cassert>
#include <cstdint>

namespace simulator {
class Quantity
{
private:
    std::uint64_t value_{0};

public:
    constexpr explicit Quantity(std::uint64_t value) noexcept
        : value_(value) {};

    constexpr std::uint64_t value() const noexcept
    {
        return value_;
    }

    constexpr bool is_zero() const noexcept
    {
        return value_ == 0;
    }

    constexpr bool operator==(const Quantity&) const = default;
    constexpr auto operator<=>(const Quantity&) const = default;
        // Precondition: other <= *this.
    constexpr Quantity operator-(const Quantity& other) const noexcept
    {
        assert(other.value_ <= value_);
        return Quantity{value_ - other.value_};
    }
};
} // namespace simulator
