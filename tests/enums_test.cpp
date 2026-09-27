#include <gtest/gtest.h>
#include <cstdint>
#include <type_traits>

#include "simulator/types/enums.hpp"

using simulator::Side;
using simulator::OrderType;
using simulator::EventType;

TEST(SideTest, HasExpectedValues)
{
    EXPECT_EQ(static_cast<std::uint8_t>(Side::Buy), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(Side::Sell), 1);
}

TEST(SideTest, UsesUint8UnderlyingType)
{
    static_assert(
        std::is_same_v<
            std::underlying_type_t<Side>,
            std::uint8_t
        >
    );
}

TEST(OrderTypeTest, HasExpectedValues)
{
    EXPECT_EQ(static_cast<std::uint8_t>(OrderType::Market), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(OrderType::Limit), 1);
}

TEST(OrderTypeTest, UsesUint8UnderlyingType)
{
    static_assert(
        std::is_same_v<
            std::underlying_type_t<OrderType>,
            std::uint8_t
        >
    );
}

TEST(EventTypeTest, HasExpectedValues)
{
    EXPECT_EQ(static_cast<std::uint8_t>(EventType::AddOrder), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(EventType::CancelOrder), 1);
    EXPECT_EQ(static_cast<std::uint8_t>(EventType::ExecuteOrder), 2);
    EXPECT_EQ(static_cast<std::uint8_t>(EventType::Trade), 3);
}

TEST(EventTypeTest, UsesUint8UnderlyingType)
{
    static_assert(
        std::is_same_v<
            std::underlying_type_t<EventType>,
            std::uint8_t
        >
    );
}