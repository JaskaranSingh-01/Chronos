#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace simulator {

// Fixed-memory log-linear histogram: values below 32 are exact, each higher power of two
// splits into 16 linear buckets (at most 6.25 % error). record() never allocates.
class LatencyHistogram
{
private:
    static constexpr int sub_bits = 4;
    static constexpr std::uint64_t sub_count = std::uint64_t{1} << sub_bits;

    std::array<std::uint64_t, 64 * sub_count> counts_{};
    std::uint64_t count_{0};
    std::uint64_t max_{0};

    // Below 16 the value itself; else 16 per power of two, by the 4 bits
    // after the leading one (so 16-31 are exact too).
    static constexpr std::size_t index(std::uint64_t value) noexcept
    {
        if (value < sub_count) {
            return static_cast<std::size_t>(value);
        }
        const int exponent = static_cast<int>(std::bit_width(value)) - 1;
        const std::uint64_t sub = (value >> (exponent - sub_bits)) & (sub_count - 1);
        return static_cast<std::size_t>(exponent - sub_bits + 1) * sub_count + sub;
    }

    // The largest value index() maps to `i`.
    static constexpr std::uint64_t upper_bound(std::size_t i) noexcept
    {
        if (i < 2 * sub_count) {
            return i;
        }
        const std::uint64_t exponent = i / sub_count + sub_bits - 1;
        const std::uint64_t sub = i % sub_count;
        return ((sub_count + sub + 1) << (exponent - sub_bits)) - 1;  // wraps to UINT64_MAX at the top
    }

public:
    void record(std::uint64_t value) noexcept
    {
        ++counts_[index(value)];
        ++count_;
        max_ = std::max(max_, value);
    }

    // Value at `fraction` (0.5 = median): its bucket's upper bound, at most 6.25 % high
    // and never above max(). 0 if nothing was recorded.
    std::uint64_t percentile(double fraction) const noexcept
    {
        if (count_ == 0) {
            return 0;
        }
        const auto wanted = static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(count_)));
        const std::uint64_t rank = std::clamp<std::uint64_t>(wanted, 1, count_);

        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= rank) {
                return std::min(upper_bound(i), max_);
            }
        }
        return max_;
    }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t max() const noexcept { return max_; }
};

} // namespace simulator
