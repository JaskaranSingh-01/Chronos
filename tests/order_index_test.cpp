#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "simulator/orders/order_index.hpp"
#include "simulator/orders/order.hpp"

namespace simulator {
namespace {

// Distinct addresses to store; the index never dereferences them.
Order orders[64];

// `count` ids, from `from` up, that hash to the same slot as `from`.
std::vector<OrderId> colliding(const OrderIndex& index, OrderId from, std::size_t count)
{
    std::vector<OrderId> ids;
    for (OrderId id = from; ids.size() < count; ++id) {
        if (index.home(id) == index.home(from)) {
            ids.push_back(id);
        }
    }
    return ids;
}

TEST(OrderIndexTest, FindsWhatWasInsertedAndNothingElse)
{
    OrderIndex index;
    EXPECT_EQ(index.find(1), nullptr);
    EXPECT_FALSE(index.erase(1));

    EXPECT_TRUE(index.insert(1, &orders[1]));
    EXPECT_TRUE(index.insert(42, &orders[2]));
    EXPECT_FALSE(index.insert(42, &orders[3]));  // duplicate id: kept as it was

    EXPECT_EQ(index.size(), 2u);
    EXPECT_EQ(index.find(1), &orders[1]);
    EXPECT_EQ(index.find(42), &orders[2]);
    EXPECT_EQ(index.find(7), nullptr);

    EXPECT_TRUE(index.erase(1));
    EXPECT_FALSE(index.erase(1));
    EXPECT_EQ(index.find(1), nullptr);
    EXPECT_EQ(index.find(42), &orders[2]);
    EXPECT_EQ(index.size(), 1u);
}

TEST(OrderIndexTest, CollidingIdsStayFindableThroughErase)
{
    OrderIndex index{8};
    const std::vector<OrderId> ids = colliding(index, 1'000, 4);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        ASSERT_TRUE(index.insert(ids[i], &orders[i]));
    }

    // Erasing the head of the probe run shifts the rest back over the hole.
    ASSERT_TRUE(index.erase(ids[0]));
    EXPECT_EQ(index.find(ids[0]), nullptr);
    EXPECT_EQ(index.find(ids[1]), &orders[1]);
    EXPECT_EQ(index.find(ids[2]), &orders[2]);
    EXPECT_EQ(index.find(ids[3]), &orders[3]);

    ASSERT_TRUE(index.erase(ids[2]));
    EXPECT_EQ(index.find(ids[1]), &orders[1]);
    EXPECT_EQ(index.find(ids[3]), &orders[3]);

    ASSERT_TRUE(index.insert(ids[0], &orders[0]));
    EXPECT_EQ(index.find(ids[0]), &orders[0]);
    EXPECT_EQ(index.size(), 3u);
}

TEST(OrderIndexTest, ProbeRunsWrapAroundTheEndOfTheTable)
{
    OrderIndex index{8};  // 16 slots
    OrderId last = 1;
    while (index.home(last) != 15) {
        ++last;
    }
    const std::vector<OrderId> ids = colliding(index, last, 3);  // slots 15, 0, 1
    OrderId zero = 1;
    while (index.home(zero) != 0) {
        ++zero;
    }

    for (std::size_t i = 0; i < ids.size(); ++i) {
        ASSERT_TRUE(index.insert(ids[i], &orders[i]));
    }
    ASSERT_TRUE(index.insert(zero, &orders[9]));  // its home is taken by the wrapped run

    ASSERT_TRUE(index.erase(ids[0]));
    EXPECT_EQ(index.find(ids[1]), &orders[1]);
    EXPECT_EQ(index.find(ids[2]), &orders[2]);
    EXPECT_EQ(index.find(zero), &orders[9]);
    ASSERT_TRUE(index.erase(ids[1]));
    EXPECT_EQ(index.find(ids[2]), &orders[2]);
    EXPECT_EQ(index.find(zero), &orders[9]);
}

// A small, often half-full table: long probe runs, wrap-around and backward shifts.
TEST(OrderIndexTest, MatchesAReferenceMapUnderChurn)
{
    OrderIndex index{8};
    const std::size_t capacity = index.capacity();
    std::unordered_map<OrderId, Order*> reference;
    std::mt19937_64 rng{1};

    for (int step = 0; step < 200'000; ++step) {
        const OrderId id = 1 + rng() % 48;
        if (reference.contains(id)) {
            ASSERT_TRUE(index.erase(id));
            reference.erase(id);
        } else if (reference.size() < capacity) {
            ASSERT_TRUE(index.insert(id, &orders[id]));
            reference.emplace(id, &orders[id]);
        }
        const OrderId probe = 1 + rng() % 48;
        const auto it = reference.find(probe);
        ASSERT_EQ(index.find(probe), it == reference.end() ? nullptr : it->second) << "step " << step;
        ASSERT_EQ(index.size(), reference.size());
    }
    EXPECT_EQ(index.capacity(), capacity);  // never grew
}

TEST(OrderIndexTest, ACapacityFromThePoolIsNeverOutgrown)
{
    OrderIndex index{1'000};
    const std::size_t capacity = index.capacity();
    ASSERT_GE(capacity, 1'000u);

    for (OrderId id = 1; id <= 1'000; ++id) {
        ASSERT_TRUE(index.insert(id * 7'919, &orders[id % 64]));
    }
    EXPECT_EQ(index.capacity(), capacity);
    for (OrderId id = 1; id <= 1'000; ++id) {
        ASSERT_EQ(index.find(id * 7'919), &orders[id % 64]);
    }
}

TEST(OrderIndexTest, GrowsWithoutACapacity)
{
    OrderIndex index;
    EXPECT_EQ(index.capacity(), 0u);

    for (OrderId id = 1; id <= 10'000; ++id) {
        ASSERT_TRUE(index.insert(id, &orders[id % 64]));
    }
    EXPECT_GE(index.capacity(), 10'000u);
    for (OrderId id = 1; id <= 10'000; ++id) {
        ASSERT_EQ(index.find(id), &orders[id % 64]);
    }
    EXPECT_EQ(index.find(10'001), nullptr);
}

} // namespace
} // namespace simulator
