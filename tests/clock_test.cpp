#include <gtest/gtest.h>

#include "simulator/clock/clock.hpp"

using simulator::Clock;
using simulator::Timestamp;

TEST(ClockTest, StartsAtZero)
{
    constexpr Clock clock{};

    EXPECT_EQ(clock.now(), Timestamp{0});
}

TEST(ClockTest, AdvanceToChangesTime)
{
    Clock clock{};

    clock.advance_to(Timestamp{1000});

    EXPECT_EQ(clock.now(), Timestamp{1000});
}

TEST(ClockTest, CanAdvanceMultipleTimes)
{
    Clock clock{};

    clock.advance_to(Timestamp{100});
    EXPECT_EQ(clock.now(), Timestamp{100});

    clock.advance_to(Timestamp{500});
    EXPECT_EQ(clock.now(), Timestamp{500});

    clock.advance_to(Timestamp{1000});
    EXPECT_EQ(clock.now(), Timestamp{1000});
}

TEST(ClockTest, CanAdvanceToSameTime)
{
    Clock clock{};

    clock.advance_to(Timestamp{1000});
    clock.advance_to(Timestamp{1000});

    EXPECT_EQ(clock.now(), Timestamp{1000});
}

TEST(ClockTest, CanMoveBackward)
{
    Clock clock{};

    clock.advance_to(Timestamp{1000});
    clock.advance_to(Timestamp{500});

    EXPECT_EQ(clock.now(), Timestamp{500});
}

TEST(ClockTest, SupportsNegativeTimestamp)
{
    Clock clock{};

    clock.advance_to(Timestamp{-100});

    EXPECT_EQ(clock.now(), Timestamp{-100});
}

TEST(ClockTest, NowDoesNotModifyClock)
{
    Clock clock{};

    clock.advance_to(Timestamp{1000});

    const Timestamp first = clock.now();
    const Timestamp second = clock.now();

    EXPECT_EQ(first, second);
    EXPECT_EQ(clock.now(), Timestamp{1000});
}