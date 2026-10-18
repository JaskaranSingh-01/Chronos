#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <span>
#include <thread>
#include <vector>

#include "chronos/server/channels.hpp"

namespace simulator::server {
namespace {

TEST(FixedQueueTest, KeepsOrderAndRefusesWhenFull)
{
    FixedQueue<int> queue{3};
    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    EXPECT_TRUE(queue.push(3));
    EXPECT_FALSE(queue.push(4));
    int value{0};
    ASSERT_TRUE(queue.pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.push(4));  // wraps around
    std::vector<int> rest;
    while (queue.pop(value)) {
        rest.push_back(value);
    }
    EXPECT_EQ(rest, (std::vector<int>{2, 3, 4}));
    EXPECT_TRUE(queue.empty());
}

struct Item
{
    std::uint64_t seq{0};
    int value{0};
};

TEST(RecordRingTest, KeepsTheLastCapacityRecordsAndReportsTheGap)
{
    RecordRing<Item> ring{4};
    std::vector<Item> out(8);
    RecordRing<Item>::Read read = ring.read(1, out);
    EXPECT_EQ(read.count, 0u);
    EXPECT_EQ(read.last_seq, 0u);

    for (int value = 1; value <= 6; ++value) {
        EXPECT_EQ(ring.publish(Item{.value = value}), static_cast<std::uint64_t>(value));
    }
    read = ring.read(1, out);  // 1 and 2 were overwritten: a gap
    EXPECT_EQ(read.first_seq, 3u);
    EXPECT_EQ(read.last_seq, 6u);
    ASSERT_EQ(read.count, 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(out[i].seq, 3 + i);
        EXPECT_EQ(out[i].value, static_cast<int>(3 + i));
    }

    read = ring.read(5, std::span<Item>{out}.first(1));  // never more than the caller's buffer
    ASSERT_EQ(read.count, 1u);
    EXPECT_EQ(out[0].seq, 5u);
    EXPECT_EQ(ring.read(7, out).count, 0u);
    EXPECT_EQ(ring.last_seq(), 6u);
}

TEST(LatestSlotTest, APublishWhileAReaderHoldsTheSlotIsDropped)
{
    LatestSlot<int> slot;
    int seen{0};
    EXPECT_FALSE(slot.read([&](const int& value) { seen = value; }));
    ASSERT_TRUE(slot.try_publish(1));

    std::promise<void> holding;
    std::promise<void> release;
    std::future<void> held = holding.get_future();
    std::future<void> released = release.get_future();
    std::thread reader{[&] {
        slot.read([&](const int& value) {
            seen = value;
            holding.set_value();
            released.wait();
        });
    }};
    held.wait();
    EXPECT_FALSE(slot.try_publish(2));  // the writer never waits for a reader
    release.set_value();
    reader.join();

    EXPECT_EQ(seen, 1);
    seen = 0;
    ASSERT_TRUE(slot.read([&](const int& value) { seen = value; }));
    EXPECT_EQ(seen, 1);  // the dropped publish was not stored
    ASSERT_TRUE(slot.try_publish(3));
    ASSERT_TRUE(slot.read([&](const int& value) { seen = value; }));
    EXPECT_EQ(seen, 3);
}

struct Pair
{
    std::uint64_t a{0};
    std::uint64_t b{0};  // always 3 * a
};

TEST(LatestSlotTest, ConcurrentReadsNeverSeeATornValue)
{
    LatestSlot<Pair> slot;
    ASSERT_TRUE(
        slot.try_publish(Pair{}));  // the final read finds a value even if a held slot drops every publish below
    std::atomic<bool> done{false};
    std::thread writer{[&] {
        for (std::uint64_t i = 1; i <= 200'000; ++i) {
            slot.try_publish(Pair{.a = i, .b = 3 * i});
        }
        done = true;
    }};
    std::uint64_t last{0};
    bool torn{false};
    bool backwards{false};
    while (!done) {
        slot.read([&](const Pair& p) {
            torn = torn || p.b != 3 * p.a;
            backwards = backwards || p.a < last;
            last = p.a;
        });
    }
    writer.join();
    EXPECT_FALSE(torn);
    EXPECT_FALSE(backwards);
    Pair latest;
    ASSERT_TRUE(slot.read([&](const Pair& p) { latest = p; }));
    EXPECT_EQ(latest.b, 3 * latest.a);
}

}  // namespace
}  // namespace simulator::server
