#include <gtest/gtest.h>
#include <limits>

#include "simulator/types/quantity.hpp"

using simulator::Quantity;

TEST(QuantityTest, DefaultValueIsZero)
{
    constexpr Quantity quantity{0};

    EXPECT_EQ(quantity.value(), 0);
}

TEST(QuantityTest, StoresValue)
{
    constexpr Quantity quantity{100};

    EXPECT_EQ(quantity.value(), 100);
}

TEST(QuantityTest, EqualityWorks)
{
    constexpr Quantity a{100};
    constexpr Quantity b{100};
    constexpr Quantity c{200};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(QuantityTest, OrderingWorks)
{
    constexpr Quantity small{10};
    constexpr Quantity large{100};

    EXPECT_LT(small, large);
    EXPECT_LE(small, large);
    EXPECT_GT(large, small);
    EXPECT_GE(large, small);
}

TEST(QuantityTest, ComparisonWorksAtCompileTime)
{
    constexpr Quantity small{10};
    constexpr Quantity large{100};

    static_assert(small < large);
    static_assert(small != large);
}

TEST(QuantityTest, SupportsMaximumValue)
{
    constexpr Quantity quantity{
        std::numeric_limits<std::uint32_t>::max()
    };

    EXPECT_EQ(
        quantity.value(),
        std::numeric_limits<std::uint32_t>::max()
    );
}