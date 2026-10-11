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
