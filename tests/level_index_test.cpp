#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>

#include "simulator/orders/level_index.hpp"

namespace simulator {
namespace {

TEST(LevelIndexTest, AKeyIsTheBookTheSideAndThePrice)
{
    LevelIndex index{16};
    ASSERT_TRUE(index.add(1, Side::Buy, Price{100}, 5));
    EXPECT_EQ(index.find(1, Side::Buy, Price{100}), 5u);
    EXPECT_EQ(index.find(1, Side::Sell, Price{100}), 0u);
    EXPECT_EQ(index.find(2, Side::Buy, Price{100}), 0u);
    EXPECT_EQ(index.find(1, Side::Buy, Price{101}), 0u);

    ASSERT_TRUE(index.add(1, Side::Sell, Price{100}, 6));
    ASSERT_TRUE(index.add(2, Side::Buy, Price{100}, 7));
    ASSERT_TRUE(index.add(1, Side::Buy, Price{-3}, 8));
    EXPECT_EQ(index.find(1, Side::Buy, Price{100}), 5u);
    EXPECT_EQ(index.find(1, Side::Sell, Price{100}), 6u);
    EXPECT_EQ(index.find(2, Side::Buy, Price{100}), 7u);
    EXPECT_EQ(index.find(1, Side::Buy, Price{-3}), 8u);
    EXPECT_EQ(index.size(), 4u);

    EXPECT_TRUE(index.erase(1, Side::Buy, Price{100}));
    EXPECT_FALSE(index.erase(1, Side::Buy, Price{100}));
    EXPECT_EQ(index.find(1, Side::Buy, Price{100}), 0u);
    EXPECT_EQ(index.find(1, Side::Sell, Price{100}), 6u);
    EXPECT_EQ(index.find(2, Side::Buy, Price{100}), 7u);
    EXPECT_EQ(index.size(), 3u);
}

// A small, often full table: probe runs long enough that keys one field apart meet, which only the full key tells
// apart.
TEST(LevelIndexTest, MatchesAReferenceUnderChurn)
{
    LevelIndex index{8};
    LevelRef reference[2][2][6]{};  // by book, side and price: the level indexed there, or 0
    std::size_t size = 0;
    std::mt19937_64 rng{3};

    for (int step = 0; step < 20'000; ++step) {
        const auto book = static_cast<SymbolId>(rng() % 2);
        const auto side = static_cast<Side>(rng() % 2);
        const auto price = static_cast<std::int64_t>(rng() % 6);
        LevelRef& entry = reference[book][static_cast<std::size_t>(side)][price];
        if (entry != 0) {
            ASSERT_TRUE(index.erase(book, side, Price{price}));
            entry = 0;
            --size;
        } else if (size < index.capacity()) {
            entry = static_cast<LevelRef>(1 + rng() % 1'000);
            ASSERT_TRUE(index.add(book, side, Price{price}, entry));
            ++size;
        }
        const auto probe_book = static_cast<SymbolId>(rng() % 2);
        const auto probe_side = static_cast<Side>(rng() % 2);
        const auto probe_price = static_cast<std::int64_t>(rng() % 6);
        ASSERT_EQ(index.find(probe_book, probe_side, Price{probe_price}),
                  reference[probe_book][static_cast<std::size_t>(probe_side)][probe_price])
            << "step " << step;
        ASSERT_EQ(index.size(), size);
    }
}

// LevelPool's largest slot, with either side, still fits the 32-bit entry.
TEST(LevelIndexTest, KeepsTheLargestLevelOnEitherSide)
{
    constexpr LevelRef largest = (LevelRef{1} << 31) - 1;
    LevelIndex index{4};
    ASSERT_TRUE(index.add(0, Side::Buy, Price{1}, largest));
    ASSERT_TRUE(index.add(0, Side::Sell, Price{1}, largest - 1));
    EXPECT_EQ(index.find(0, Side::Buy, Price{1}), largest);
    EXPECT_EQ(index.find(0, Side::Sell, Price{1}), largest - 1);
}

TEST(LevelIndexTest, AFullIndexRefusesAnAdd)
{
    LevelIndex index{8};
    ASSERT_EQ(index.capacity(), 8u);
    for (std::int64_t price = 1; price <= 8; ++price) {
        ASSERT_TRUE(index.add(0, Side::Buy, Price{price}, static_cast<LevelRef>(price)));
    }
    EXPECT_TRUE(index.full());
    EXPECT_FALSE(index.add(0, Side::Buy, Price{9}, 9));
    EXPECT_EQ(index.find(0, Side::Buy, Price{9}), 0u);
    EXPECT_EQ(index.size(), 8u);
}

} // namespace
} // namespace simulator
