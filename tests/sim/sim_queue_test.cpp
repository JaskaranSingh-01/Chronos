#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "simulator/sim/sim_event.hpp"

namespace simulator {
namespace {

SimEvent timer(Timestamp at, std::uint64_t timer_id)
{
    return SimEvent{.timestamp = at, .type = SimEventType::Timer, .timer_id = timer_id};
}

std::vector<std::uint64_t> drain_timer_ids(SimQueue& queue)
{
    std::vector<std::uint64_t> ids;
    while (!queue.empty()) {
        ids.push_back(queue.top().timer_id);
        queue.pop();
    }
    return ids;
}

TEST(SimQueueTest, PopsInTimestampOrder)
{
    SimQueue queue{8};
    queue.push(timer(300, 3));
    queue.push(timer(100, 1));
    queue.push(timer(200, 2));

    EXPECT_EQ(drain_timer_ids(queue), (std::vector<std::uint64_t>{1, 2, 3}));
}

TEST(SimQueueTest, EqualTimestampsPopInPushOrder)
{
    SimQueue queue{8};
    for (std::uint64_t id = 1; id <= 5; ++id) {
        queue.push(timer(100, id));
    }
    queue.push(timer(50, 0));

    EXPECT_EQ(drain_timer_ids(queue), (std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5}));
}

TEST(SimQueueTest, AssignsMonotonicSequenceOnPushIgnoringTheCallersValue)
{
    SimQueue queue{2};  // capacity is a reservation, not a limit
    SimEvent event = timer(100, 0);
    event.sequence = 999;
    queue.push(event);
    queue.push(timer(100, 1));
    queue.push(timer(100, 2));

    std::vector<std::uint64_t> sequences;
    while (!queue.empty()) {
        sequences.push_back(queue.top().sequence);
        queue.pop();
    }
    EXPECT_EQ(sequences, (std::vector<std::uint64_t>{0, 1, 2}));
}

TEST(SimQueueTest, CarriesThePayloadOfEachEventType)
{
    SimQueue queue{4};
    queue.push(SimEvent{
        .timestamp = 10,
        .type = SimEventType::OrderArrive,
        .order = OrderRequest{.id = 7, .side = Side::Sell, .price = Price{1'000'000}, .quantity = Quantity{300}}});
    queue.push(SimEvent{.timestamp = 20, .type = SimEventType::CancelArrive, .order_id = 7});

    ASSERT_FALSE(queue.empty());
    EXPECT_EQ(queue.top().type, SimEventType::OrderArrive);
    EXPECT_EQ(queue.top().order.id, 7u);
    EXPECT_EQ(queue.top().order.side, Side::Sell);
    EXPECT_EQ(queue.top().order.quantity, Quantity{300});
    EXPECT_EQ(queue.top().order.time_in_force, TimeInForce::Ioc);
    queue.pop();

    EXPECT_EQ(queue.top().type, SimEventType::CancelArrive);
    EXPECT_EQ(queue.top().order_id, 7u);
    queue.pop();
    EXPECT_TRUE(queue.empty());
}

} // namespace
} // namespace simulator
