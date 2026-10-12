#include <gtest/gtest.h>

#include <cstdint>

#include "simulator/sim/order_gateway.hpp"

namespace simulator {
namespace {

constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};
constexpr LatencyConfig kLatency{.order_latency = 20'000, .report_latency = 15'000};

struct Fixture
{
    SimQueue queue{16};
    Portfolio portfolio{kEquity,
                        RiskLimits{.max_position = 1'000, .max_order_quantity = Quantity{500},
                                   .max_open_orders = 8},
                        FeeSchedule{}};
    OrderGateway gateway{queue, portfolio, kLatency};
};

TEST(OrderGatewayTest, AcceptedOrderArrivesAtNowPlusOrderLatency)
{
    Fixture f;
    f.gateway.set_now(1'000);

    OrderId id{0};
    ASSERT_EQ(f.gateway.send(Side::Buy, Price{1'000'100}, Quantity{200}, TimeInForce::Ioc, id),
              RiskCheck::Accepted);
    EXPECT_EQ(id, 1u);
    EXPECT_EQ(f.portfolio.open_quantity(Side::Buy), 200u);  // reserved synchronously

    ASSERT_FALSE(f.queue.empty());
    const SimEvent& arrive = f.queue.top();
    EXPECT_EQ(arrive.type, SimEventType::OrderArrive);
    EXPECT_EQ(arrive.timestamp, 21'000);
    EXPECT_EQ(arrive.order.id, 1u);
    EXPECT_EQ(arrive.order.side, Side::Buy);
    EXPECT_EQ(arrive.order.price, Price{1'000'100});
    EXPECT_EQ(arrive.order.quantity, Quantity{200});
    EXPECT_EQ(arrive.order.time_in_force, TimeInForce::Ioc);
}

TEST(OrderGatewayTest, RiskRejectSchedulesNothingAndKeepsTheId)
{
    Fixture f;
    OrderId id{77};

    EXPECT_EQ(f.gateway.send(Side::Buy, Price{100}, Quantity{501}, TimeInForce::Ioc, id),
              RiskCheck::OrderTooLarge);
    EXPECT_EQ(f.gateway.send(Side::Sell, Price{100}, Quantity{0}, TimeInForce::Ioc, id),
              RiskCheck::InvalidOrder);
    EXPECT_EQ(id, 77u);
    EXPECT_TRUE(f.queue.empty());
    EXPECT_EQ(f.portfolio.open_orders(), 0u);
}

TEST(OrderGatewayTest, IdsAreUniqueAndMonotonicAcrossRejects)
{
    Fixture f;
    OrderId previous{0};

    for (int i = 0; i < 6; ++i) {
        OrderId id{0};
        if (i == 3) {  // a reject in the middle consumes no id
            ASSERT_EQ(f.gateway.send(Side::Buy, Price{100}, Quantity{999}, TimeInForce::Ioc, id),
                      RiskCheck::OrderTooLarge);
            continue;
        }
        ASSERT_EQ(f.gateway.send(i % 2 ? Side::Buy : Side::Sell, Price{100}, Quantity{10}, TimeInForce::Ioc, id),
                  RiskCheck::Accepted);
        EXPECT_EQ(id, previous + 1);
        previous = id;
    }
    EXPECT_EQ(previous, 5u);
}

TEST(OrderGatewayTest, SendThenCancelInOneCallbackArriveInCallOrder)
{
    Fixture f;
    f.gateway.set_now(500);

    OrderId id{0};
    ASSERT_EQ(f.gateway.send(Side::Sell, Price{100}, Quantity{10}, TimeInForce::Ioc, id), RiskCheck::Accepted);
    f.gateway.cancel(id);

    ASSERT_FALSE(f.queue.empty());
    EXPECT_EQ(f.queue.top().type, SimEventType::OrderArrive);
    EXPECT_EQ(f.queue.top().timestamp, 20'500);
    f.queue.pop();
    ASSERT_FALSE(f.queue.empty());
    EXPECT_EQ(f.queue.top().type, SimEventType::CancelArrive);
    EXPECT_EQ(f.queue.top().timestamp, 20'500);
    EXPECT_EQ(f.queue.top().order_id, id);
}

TEST(OrderGatewayTest, TimersAreNeverInThePast)
{
    Fixture f;
    f.gateway.set_now(1'000);

    EXPECT_FALSE(f.gateway.set_timer(999, 1));
    EXPECT_TRUE(f.queue.empty());

    EXPECT_TRUE(f.gateway.set_timer(1'000, 7));
    ASSERT_FALSE(f.queue.empty());
    EXPECT_EQ(f.queue.top().type, SimEventType::Timer);
    EXPECT_EQ(f.queue.top().timestamp, 1'000);
    EXPECT_EQ(f.queue.top().timer_id, 7u);
}

} // namespace
} // namespace simulator
