#include <cstdint>
#include <gtest/gtest.h>
#include <type_traits>

#include "simulator/types/enums.hpp"

using simulator::Side;

TEST(SideTest, HasExpectedValues)
{
    EXPECT_EQ(static_cast<std::uint8_t>(Side::Buy), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(Side::Sell), 1);
}

TEST(SideTest, UsesUint8UnderlyingType)
{
    static_assert(std::is_same_v<std::underlying_type_t<Side>, std::uint8_t>);
}
