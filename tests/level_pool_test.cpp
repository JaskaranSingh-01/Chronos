#include <gtest/gtest.h>

#include <set>

#include "simulator/orders/level_pool.hpp"

namespace simulator {
namespace {

void expect_fresh(const PriceLevel& level, Price price)
{
    EXPECT_EQ(level.price(), price);
    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.front(), nullptr);
    EXPECT_EQ(level.order_count(), 0u);
    EXPECT_EQ(level.total_quantity(), 0u);
    EXPECT_EQ(level.better(), nullptr);
    EXPECT_EQ(level.worse(), nullptr);
}

TEST(LevelPoolTest, HandsOutEmptyUnlinkedLevelsAtTheirPriceAndNeverSlotZero)
{
    LevelPool pool{3};
    std::set<LevelRef> refs;
    for (std::int64_t price = 100; price < 103; ++price) {
        PriceLevel* level = pool.acquire(Price{price});
        ASSERT_NE(level, nullptr);
        expect_fresh(*level, Price{price});
        EXPECT_EQ(&pool.at(pool.ref(*level)), level);
        refs.insert(pool.ref(*level));
    }
    EXPECT_EQ(refs, (std::set<LevelRef>{1, 2, 3}));
    EXPECT_EQ(pool.in_use(), 3u);
    EXPECT_TRUE(pool.full());
}

TEST(LevelPoolTest, AFullPoolHandsOutNothingUntilALevelIsReleased)
{
    LevelPool pool{2};
    PriceLevel* first = pool.acquire(Price{100});
    PriceLevel* second = pool.acquire(Price{101});
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(pool.acquire(Price{102}), nullptr);

    pool.release(*first);
    EXPECT_EQ(pool.in_use(), 1u);
    PriceLevel* again = pool.acquire(Price{103});
    EXPECT_EQ(again, first);
    expect_fresh(*again, Price{103});
    EXPECT_EQ(pool.in_use(), 2u);
}

// A released slot is handed out before one never used, so the pool touches only as many slots as were ever live.
TEST(LevelPoolTest, ReusesAReleasedSlotBeforeAnUntouchedOne)
{
    LevelPool pool{1'000};
    for (int i = 0; i < 100; ++i) {
        PriceLevel* level = pool.acquire(Price{i});
        ASSERT_NE(level, nullptr);
        EXPECT_EQ(pool.ref(*level), 1u);
        pool.release(*level);
    }
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.bytes(), 1'001 * sizeof(PriceLevel));
}

} // namespace
} // namespace simulator
