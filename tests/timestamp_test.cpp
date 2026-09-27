#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>
#include <limits>

#include "simulator/types/timestamp.hpp"

using simulator::Timestamp;

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