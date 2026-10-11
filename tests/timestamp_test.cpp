#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <type_traits>
#include <limits>

#include "simulator/types/timestamp.hpp"

using simulator::Timestamp;
using simulator::session_midnight_ns;
using simulator::RTH_OPEN_NS;
using simulator::RTH_CLOSE_NS;

TEST(TimestampTest, IsInt64)
{
    static_assert(
        std::is_same_v<Timestamp, std::int64_t>
    );
}

TEST(TimestampTest, StoresValue)
{
    constexpr Timestamp timestamp = 1000;

    EXPECT_EQ(timestamp, 1000);
}

TEST(TimestampTest, SupportsNegativeValues)
{
    constexpr Timestamp timestamp = -1000;

    EXPECT_EQ(timestamp, -1000);
}

TEST(TimestampTest, SupportsMaximumValue)
{
    constexpr Timestamp timestamp =
        std::numeric_limits<std::int64_t>::max();

    EXPECT_EQ(
        timestamp,
        std::numeric_limits<std::int64_t>::max()
    );
}

TEST(TimestampTest, SupportsMinimumValue)
{
    constexpr Timestamp timestamp =
        std::numeric_limits<std::int64_t>::min();

    EXPECT_EQ(
        timestamp,
        std::numeric_limits<std::int64_t>::min()
    );
}

TEST(TimestampTest, SessionMidnight)
{
    using namespace std::chrono;

    // 2019-12-30 is in EST (UTC-05:00).
    constexpr Timestamp winter =
        session_midnight_ns(2019y / December / 30, -5h);

    EXPECT_EQ(winter, 1'577'682'000'000'000'000);

    // 2012-06-21 is in EDT (UTC-04:00).
    EXPECT_EQ(
        session_midnight_ns(2012y / June / 21, -4h),
        1'340'251'200'000'000'000
    );

    // 09:30 and 16:00 EST are 14:30 and 21:00 UTC.
    EXPECT_EQ(winter + RTH_OPEN_NS, 1'577'716'200'000'000'000);
    EXPECT_EQ(winter + RTH_CLOSE_NS, 1'577'739'600'000'000'000);
}

TEST(TimestampTest, UsEasternUtcOffset)
{
    using namespace std::chrono;
    using simulator::us_eastern_utc_offset;

    EXPECT_EQ(us_eastern_utc_offset(2019y / December / 30), -5h);
    EXPECT_EQ(us_eastern_utc_offset(2012y / June / 21), -4h);

    // 2019's transitions: Sunday 10 March and Sunday 3 November, both at
    // 02:00 local. Midnight is before the switch, so it keeps the old offset.
    EXPECT_EQ(us_eastern_utc_offset(2019y / March / 9), -5h);
    EXPECT_EQ(us_eastern_utc_offset(2019y / March / 10), -5h);
    EXPECT_EQ(us_eastern_utc_offset(2019y / March / 11), -4h);
    EXPECT_EQ(us_eastern_utc_offset(2019y / November / 2), -4h);
    EXPECT_EQ(us_eastern_utc_offset(2019y / November / 3), -4h);
    EXPECT_EQ(us_eastern_utc_offset(2019y / November / 4), -5h);

    // Monday after each switch: midnight 04:00 UTC in summer, 05:00 in winter.
    EXPECT_EQ(session_midnight_ns(2019y / March / 11, us_eastern_utc_offset(2019y / March / 11)),
              1'552'276'800'000'000'000);
    EXPECT_EQ(session_midnight_ns(2019y / November / 4, us_eastern_utc_offset(2019y / November / 4)),
              1'572'843'600'000'000'000);
}
