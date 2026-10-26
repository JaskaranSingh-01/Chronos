#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "simulator/market/spsc_ring.hpp"

namespace simulator {
namespace {

// A value whose halves must agree: a torn slot would show.
struct Item
{
    std::uint64_t seq{0};
    std::uint64_t check{0};
};

TEST(SpscRingTest, HandsValuesOverInOrderUntilFullAndAcrossTheWrap)
{
    SpscRing<Item> ring{4};
    Item item;
    EXPECT_FALSE(ring.try_pop(item));
    for (std::uint64_t round = 0; round < 3; ++round) {  // the indices pass the array's end twice
        for (std::uint64_t i = 0; i < 4; ++i) {
            ASSERT_TRUE(ring.try_push({.seq = 4 * round + i, .check = ~(4 * round + i)}));
        }
        EXPECT_FALSE(ring.try_push({.seq = 99}));  // full
        for (std::uint64_t i = 0; i < 4; ++i) {
            ASSERT_TRUE(ring.try_pop(item));
            EXPECT_EQ(item.seq, 4 * round + i);
        }
        EXPECT_FALSE(ring.try_pop(item));  // empty
    }
    EXPECT_EQ(ring.capacity(), 4u);
}

// Many values through a small ring: each side waits on the other all the time. Run under TSan in CI.
TEST(SpscRingTest, TwoThreadsPassValuesInOrderThroughASmallRing)
{
    constexpr std::uint64_t count = 200'000;
    SpscRing<Item> ring{16};

    std::thread producer{[&] {
        for (std::uint64_t i = 1; i <= count; ++i) {
            while (!ring.try_push({.seq = i, .check = ~i})) {
                std::this_thread::yield();
            }
        }
    }};

    std::uint64_t expected = 1;
    std::uint64_t wrong = 0;  // checked after the join: a failed ASSERT here would leave the producer joinable
    Item item;
    while (expected <= count) {
        if (!ring.try_pop(item)) {
            std::this_thread::yield();
            continue;
        }
        wrong += item.seq != expected || item.check != ~expected;
        ++expected;
    }
    producer.join();
    EXPECT_EQ(wrong, 0u);
    EXPECT_FALSE(ring.try_pop(item));
}

} // namespace
} // namespace simulator
