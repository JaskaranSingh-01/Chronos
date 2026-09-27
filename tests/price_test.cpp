#include <gtest/gtest.h>

#include "simulator/types/price.hpp"

using simulator::Price;

TEST(PriceTest, DefaultValueIsZero)
{
    constexpr Price price{};

    EXPECT_EQ(price.ticks, 0);
}

TEST(PriceTest, StoresTicks)
{
    constexpr Price price{100};

    EXPECT_EQ(price.ticks, 100);
}

TEST(PriceTest, EqualityWorks)
{
    constexpr Price a{100};
    constexpr Price b{100};
    constexpr Price c{200};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(PriceTest, OrderingWorks)
{
    constexpr Price low{100};
    constexpr Price high{200};

    EXPECT_LT(low, high);
    EXPECT_LE(low, high);

    EXPECT_GT(high, low);
    EXPECT_GE(high, low);
}

TEST(PriceTest, ComparisonWorksAtCompileTime)
{
    constexpr Price low{100};
    constexpr Price high{200};

    static_assert(low < high);
    static_assert(low != high);
}