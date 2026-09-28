#pragma once

#include <cstdint>

namespace simulator
{
    class Quantity
    {
    private:
        std::uint32_t value_{0};

    public:
        constexpr explicit Quantity(std::uint32_t value) noexcept
            : value_(value) {};

        constexpr std::uint32_t value() const noexcept
        {
            return value_;
        }

        constexpr bool is_zero() const noexcept
        {
            return value_ == 0;
        }

        constexpr bool operator==(const Quantity &) const = default;
        constexpr auto operator<=>(const Quantity &) const = default;
        constexpr Quantity operator-(const Quantity& other) const noexcept {
            return Quantity{value_ - other.value_};
        }
        // constexpr Quantity subtract(Quantity other) const noexcept {
        //     return Quantity{value_ - other.value_};
        // }

    };
} // namespace simulator
