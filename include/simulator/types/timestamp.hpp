#pragma once

#include <chrono>
#include <cstdint>

namespace simulator
{
    // Nanoseconds since the Unix epoch, UTC.
    using Timestamp = std::int64_t;

    // Regular trading hours as ns after session midnight (09:30 and 16:00 exchange-local).
    inline constexpr Timestamp RTH_OPEN_NS = 34'200'000'000'000;
    inline constexpr Timestamp RTH_CLOSE_NS = 57'600'000'000'000;

    // Epoch ns of 00:00 exchange-local on `date`. No tz database: utc_offset comes
    // from config (e.g. -05:00 for New York in winter) and is constant within a day.
    constexpr Timestamp session_midnight_ns(
        std::chrono::year_month_day date,
        std::chrono::minutes utc_offset) noexcept
    {
        const auto midnight = std::chrono::sys_days{date} - utc_offset;

        return std::chrono::nanoseconds{midnight.time_since_epoch()}.count();
    }

    // America/New_York's UTC offset at 00:00 local on `date`, the offset
    // session_midnight_ns needs: -04:00 (EDT) or -05:00 (EST). DST starts on
    // the 2nd Sunday of March and ends on the 1st Sunday of November, both at
    // 02:00 local, so midnight on either Sunday still has the old offset.
    // ponytail: US rule since 2007 only (earlier: 1st Sunday of April to last
    // Sunday of October); pass --utc-offset for older dates.
    constexpr std::chrono::minutes us_eastern_utc_offset(std::chrono::year_month_day date) noexcept
    {
        using namespace std::chrono;

        const sys_days day{date};
        const sys_days dst_start{date.year() / March / Sunday[2]};
        const sys_days dst_end{date.year() / November / Sunday[1]};

        return day > dst_start && day <= dst_end ? hours{-4} : hours{-5};
    }

} // namespace simulator
