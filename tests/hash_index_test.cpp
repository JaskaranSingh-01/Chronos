#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "simulator/orders/hash_index.hpp"

namespace simulator {
namespace {

struct Entry
{
    std::uint64_t key{0};
    std::uint32_t value{0};  // 0: a free slot

    bool empty() const noexcept
    {
        return value == 0;
    }

    std::uint64_t hash() const noexcept
    {
        return key;
    }
};

using Index = HashIndex<Entry>;

// The value stored under `key`; 0 if absent.
std::uint32_t find(const Index& index, std::uint64_t key)
{
    const Entry* entry = index.find(key, [key](const Entry& e) { return e.key == key; });
    return entry != nullptr ? entry->value : 0;
}

bool erase(Index& index, std::uint64_t key)
{
    return index.erase(key, [key](const Entry& e) { return e.key == key; });
}

// `count` keys, from `from` up, whose home is the home of `from`.
std::vector<std::uint64_t> colliding(const Index& index, std::uint64_t from, std::size_t count)
{
    std::vector<std::uint64_t> keys;
    for (std::uint64_t key = from; keys.size() < count; ++key) {
        if (index.home(key) == index.home(from)) {
            keys.push_back(key);
        }
    }
    return keys;
}

TEST(HashIndexTest, FindsWhatWasAddedAndNothingElse)
{
    Index index{8};
    EXPECT_EQ(find(index, 1), 0u);
    EXPECT_FALSE(erase(index, 1));

    EXPECT_TRUE(index.add({.key = 1, .value = 10}));
    EXPECT_TRUE(index.add({.key = 42, .value = 20}));
    EXPECT_EQ(index.size(), 2u);
    EXPECT_EQ(find(index, 1), 10u);
    EXPECT_EQ(find(index, 42), 20u);
    EXPECT_EQ(find(index, 7), 0u);

    EXPECT_TRUE(erase(index, 1));
    EXPECT_FALSE(erase(index, 1));
    EXPECT_EQ(find(index, 1), 0u);
    EXPECT_EQ(find(index, 42), 20u);
    EXPECT_EQ(index.size(), 1u);
}

TEST(HashIndexTest, CollidingKeysStayFindableThroughErase)
{
    Index index{8};
    const std::vector<std::uint64_t> keys = colliding(index, 1'000, 4);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        ASSERT_TRUE(index.add({.key = keys[i], .value = static_cast<std::uint32_t>(i + 1)}));
    }

    // Erasing the head of the probe run shifts the rest back over the hole.
    ASSERT_TRUE(erase(index, keys[0]));
    EXPECT_EQ(find(index, keys[0]), 0u);
    EXPECT_EQ(find(index, keys[1]), 2u);
    EXPECT_EQ(find(index, keys[2]), 3u);
    EXPECT_EQ(find(index, keys[3]), 4u);

    ASSERT_TRUE(erase(index, keys[2]));
    EXPECT_EQ(find(index, keys[1]), 2u);
    EXPECT_EQ(find(index, keys[3]), 4u);

    ASSERT_TRUE(index.add({.key = keys[0], .value = 1}));
    EXPECT_EQ(find(index, keys[0]), 1u);
    EXPECT_EQ(index.size(), 3u);
}

TEST(HashIndexTest, ProbeRunsWrapAroundTheEndOfTheTable)
{
    Index index{8};  // 16 slots
    std::uint64_t last = 1;
    while (index.home(last) != 15) {
        ++last;
    }
    const std::vector<std::uint64_t> keys = colliding(index, last, 3);  // slots 15, 0, 1
    std::uint64_t zero = 1;
    while (index.home(zero) != 0) {
        ++zero;
    }

    for (std::size_t i = 0; i < keys.size(); ++i) {
        ASSERT_TRUE(index.add({.key = keys[i], .value = static_cast<std::uint32_t>(i + 1)}));
    }
    ASSERT_TRUE(index.add({.key = zero, .value = 9}));  // its home is taken by the wrapped run

    ASSERT_TRUE(erase(index, keys[0]));
    EXPECT_EQ(find(index, keys[1]), 2u);
    EXPECT_EQ(find(index, keys[2]), 3u);
    EXPECT_EQ(find(index, zero), 9u);
    ASSERT_TRUE(erase(index, keys[1]));
    EXPECT_EQ(find(index, keys[2]), 3u);
    EXPECT_EQ(find(index, zero), 9u);
}

// A small table, often full: long probe runs, wrap-around and backward shifts.
TEST(HashIndexTest, MatchesAReferenceMapUnderChurn)
{
    Index index{8};
    std::unordered_map<std::uint64_t, std::uint32_t> reference;
    std::mt19937_64 rng{1};

    for (int step = 0; step < 200'000; ++step) {
        const std::uint64_t key = 1 + rng() % 48;
        if (reference.contains(key)) {
            ASSERT_TRUE(erase(index, key));
            reference.erase(key);
        } else if (reference.size() < index.capacity()) {
            const auto value = static_cast<std::uint32_t>(1 + rng() % 1'000);
            ASSERT_TRUE(index.add({.key = key, .value = value}));
            reference.emplace(key, value);
        }
        const std::uint64_t probe = 1 + rng() % 48;
        const auto it = reference.find(probe);
        ASSERT_EQ(find(index, probe), it == reference.end() ? 0u : it->second) << "step " << step;
        ASSERT_EQ(index.size(), reference.size());
    }
}

TEST(HashIndexTest, AFullIndexRefusesAnAddAndChangesNothing)
{
    Index index{8};
    ASSERT_EQ(index.capacity(), 8u);
    for (std::uint32_t key = 1; key <= 8; ++key) {
        ASSERT_TRUE(index.add({.key = key, .value = key}));
    }
    EXPECT_TRUE(index.full());
    EXPECT_FALSE(index.add({.key = 9, .value = 9}));
    EXPECT_EQ(index.size(), 8u);
    EXPECT_EQ(find(index, 9), 0u);
    for (std::uint32_t key = 1; key <= 8; ++key) {
        EXPECT_EQ(find(index, key), key);
    }

    ASSERT_TRUE(erase(index, 3));
    EXPECT_TRUE(index.add({.key = 9, .value = 9}));
    EXPECT_EQ(find(index, 9), 9u);
}

TEST(HashIndexTest, HoldsItsCapacityInAtLeastTwiceAsManySlots)
{
    EXPECT_EQ(Index{1'000}.capacity(), 1'024u);
    EXPECT_EQ(Index{1'000}.bytes(), 2'048 * sizeof(Entry));
    EXPECT_EQ(Index{4'194'304}.capacity(), 4'194'304u);
    EXPECT_EQ(Index{0}.capacity(), 8u);  // 16 slots at least
}

} // namespace
} // namespace simulator
