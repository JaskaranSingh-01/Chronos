#include <gtest/gtest.h>

#include "simulator/simulator.hpp"

namespace simulator {
namespace {

TEST(SimulatorTest, SubmitOrderWithNoLiquidityBecomesResting)
{
    Simulator simulator{10};

    Order* order = simulator.submit_order(
        1,
        Side::Buy,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(order, nullptr);

    EXPECT_EQ(order->state(), OrderState::Active);
    EXPECT_EQ(order->remaining_quantity(), Quantity{100});

    EXPECT_EQ(
        simulator.order_book().find(1),
        order
    );

    EXPECT_EQ(
        simulator.order_book().best_bid(),
        order
    );

    EXPECT_EQ(simulator.orders_in_use(), 1);
    EXPECT_EQ(simulator.orders_available(), 9);

    EXPECT_TRUE(simulator.trades().empty());
}

TEST(SimulatorTest, SubmitOrdersAndMatch)
{
    Simulator simulator{10};

    Order* sell = simulator.submit_order(
        1,
        Side::Sell,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(sell, nullptr);

    Order* buy = simulator.submit_order(
        2,
        Side::Buy,
        Price{101},
        Quantity{100},
        Timestamp{20}
    );

    ASSERT_NE(buy, nullptr);

    ASSERT_EQ(simulator.trades().size(), 1);

    const Trade& trade = simulator.trades()[0];

    EXPECT_EQ(trade.buy_order_id, 2);
    EXPECT_EQ(trade.sell_order_id, 1);
    EXPECT_EQ(trade.price, Price{100});
    EXPECT_EQ(trade.quantity, Quantity{100});

    EXPECT_EQ(buy->state(), OrderState::Filled);
    EXPECT_EQ(sell->state(), OrderState::Filled);

    EXPECT_EQ(simulator.order_book().find(1), nullptr);
    EXPECT_EQ(simulator.order_book().find(2), nullptr);
}

TEST(SimulatorTest, PartialFillLeavesIncomingOrderResting)
{
    Simulator simulator{10};

    Order* sell = simulator.submit_order(
        1,
        Side::Sell,
        Price{100},
        Quantity{50},
        Timestamp{10}
    );

    ASSERT_NE(sell, nullptr);

    Order* buy = simulator.submit_order(
        2,
        Side::Buy,
        Price{101},
        Quantity{100},
        Timestamp{20}
    );

    ASSERT_NE(buy, nullptr);

    ASSERT_EQ(simulator.trades().size(), 1);

    EXPECT_EQ(
        simulator.trades()[0].quantity,
        Quantity{50}
    );

    EXPECT_EQ(
        sell->state(),
        OrderState::Filled
    );

    EXPECT_EQ(
        buy->state(),
        OrderState::PartiallyFilled
    );

    EXPECT_EQ(
        buy->remaining_quantity(),
        Quantity{50}
    );

    EXPECT_EQ(
        simulator.order_book().find(2),
        buy
    );

    EXPECT_EQ(
        simulator.order_book().best_bid(),
        buy
    );
}

TEST(SimulatorTest, ReturnsNullWhenOrderPoolIsExhausted)
{
    Simulator simulator{1};

    Order* first = simulator.submit_order(
        1,
        Side::Buy,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(first, nullptr);

    Order* second = simulator.submit_order(
        2,
        Side::Buy,
        Price{101},
        Quantity{100},
        Timestamp{20}
    );

    EXPECT_EQ(second, nullptr);

    EXPECT_EQ(simulator.orders_in_use(), 1);
    EXPECT_EQ(simulator.orders_available(), 0);
}

TEST(SimulatorTest, FilledOrderCanBeReleasedBackToPool)
{
    Simulator simulator{2};

    Order* sell = simulator.submit_order(
        1,
        Side::Sell,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(sell, nullptr);

    Order* buy = simulator.submit_order(
        2,
        Side::Buy,
        Price{101},
        Quantity{100},
        Timestamp{20}
    );

    ASSERT_NE(buy, nullptr);

    EXPECT_EQ(
        buy->state(),
        OrderState::Filled
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        2
    );

    EXPECT_TRUE(
        simulator.release_order(*buy)
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        1
    );

    EXPECT_EQ(
        simulator.orders_available(),
        1
    );
}

TEST(SimulatorTest, ReleasesBothFilledOrdersAfterMatch)
{
    Simulator simulator{2};

    ASSERT_NE(
        simulator.submit_order(
            1,
            Side::Sell,
            Price{100},
            Quantity{100},
            Timestamp{10}
        ),
        nullptr
    );

    ASSERT_NE(
        simulator.submit_order(
            2,
            Side::Buy,
            Price{101},
            Quantity{100},
            Timestamp{20}
        ),
        nullptr
    );

    EXPECT_EQ(simulator.orders_in_use(), 2);
    EXPECT_EQ(simulator.release_filled_orders(), 2);
    EXPECT_EQ(simulator.orders_in_use(), 0);
    EXPECT_EQ(simulator.orders_available(), 2);
}

TEST(SimulatorTest, DirectReleaseRemovesFilledOrderFromPendingCleanup)
{
    Simulator simulator{2};

    ASSERT_NE(
        simulator.submit_order(
            1,
            Side::Sell,
            Price{100},
            Quantity{100},
            Timestamp{10}
        ),
        nullptr
    );

    Order* buy = simulator.submit_order(
        2,
        Side::Buy,
        Price{101},
        Quantity{100},
        Timestamp{20}
    );

    ASSERT_NE(buy, nullptr);
    ASSERT_TRUE(simulator.release_order(*buy));

    EXPECT_EQ(simulator.release_filled_orders(), 1);
    EXPECT_EQ(simulator.orders_in_use(), 0);
    EXPECT_EQ(simulator.orders_available(), 2);
}

TEST(SimulatorTest, ActiveOrderCannotBeReleased)
{
    Simulator simulator{2};

    Order* order = simulator.submit_order(
        1,
        Side::Buy,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(order, nullptr);

    EXPECT_FALSE(
        simulator.release_order(*order)
    );

    EXPECT_EQ(simulator.orders_in_use(), 1);
}

TEST(SimulatorTest, CancelsRestingOrder)
{
    Simulator simulator{10};

    Order* order = simulator.submit_order(
        1,
        Side::Buy,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(order, nullptr);

    EXPECT_EQ(
        simulator.orders_in_use(),
        1
    );

    ASSERT_TRUE(
        simulator.cancel_order(1)
    );

    EXPECT_EQ(
        order->state(),
        OrderState::Cancelled
    );

    EXPECT_EQ(
        simulator.order_book().find(1),
        nullptr
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        0
    );

    EXPECT_EQ(
        simulator.orders_available(),
        10
    );
}

TEST(SimulatorTest, CancellingUnknownOrderFails)
{
    Simulator simulator{10};

    EXPECT_FALSE(
        simulator.cancel_order(999)
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        0
    );
}

TEST(SimulatorTest, FilledOrderCannotBeCancelled)
{
    Simulator simulator{10};

    Order* sell = simulator.submit_order(
        1,
        Side::Sell,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(sell, nullptr);

    Order* buy = simulator.submit_order(
        2,
        Side::Buy,
        Price{101},
        Quantity{100},
        Timestamp{20}
    );

    ASSERT_NE(buy, nullptr);

    EXPECT_EQ(
        sell->state(),
        OrderState::Filled
    );

    EXPECT_FALSE(
        simulator.cancel_order(1)
    );
}

TEST(SimulatorTest, RejectsZeroQuantityWithoutLeakingPoolSlot)
{
    Simulator simulator{10};

    EXPECT_EQ(simulator.submit_order(1, Side::Buy, Price{100}, Quantity{0}, Timestamp{10}), nullptr);
    EXPECT_EQ(simulator.orders_in_use(), 0u);
}

TEST(SimulatorTest, RejectsDuplicateRestingIdWithoutTradingOrLeaking)
{
    Simulator simulator{10};

    ASSERT_NE(simulator.submit_order(1, Side::Buy, Price{100}, Quantity{10}, Timestamp{10}), nullptr);

    // Same id, crossing price: must not trade against its namesake.
    EXPECT_EQ(simulator.submit_order(1, Side::Sell, Price{100}, Quantity{5}, Timestamp{20}), nullptr);

    EXPECT_TRUE(simulator.trades().empty());
    EXPECT_EQ(simulator.orders_in_use(), 1u);
}

TEST(SimulatorTest, ReplayAddRejectsZeroQuantity)
{
    Simulator simulator{10};

    EXPECT_FALSE(simulator.replay_add_order(1, Side::Sell, Price{100}, Quantity{0}, Timestamp{10}));
    EXPECT_EQ(simulator.orders_in_use(), 0u);
}

TEST(SimulatorReplayTest, ReplaceMovesOrderAndLosesPriority)
{
    Simulator simulator{10};
    ASSERT_TRUE(simulator.replay_add_order(1, Side::Buy, Price{100}, Quantity{10}, Timestamp{1}));
    ASSERT_TRUE(simulator.replay_add_order(2, Side::Buy, Price{100}, Quantity{10}, Timestamp{2}));

    // Replace 1 -> 3 at the same price: 3 must now queue behind 2.
    ASSERT_TRUE(simulator.replay_replace_order(1, 3, Price{100}, Quantity{5}, Timestamp{3}));

    EXPECT_EQ(simulator.order_book().find(1), nullptr);
    ASSERT_NE(simulator.order_book().find(3), nullptr);
    EXPECT_EQ(simulator.order_book().best_bid()->id(), 2u);
    EXPECT_EQ(simulator.orders_in_use(), 2u);
}

TEST(SimulatorReplayTest, RejectedReplaceKeepsOriginalOrder)
{
    Simulator simulator{10};
    ASSERT_TRUE(simulator.replay_add_order(1, Side::Buy, Price{100}, Quantity{10}, Timestamp{1}));

    EXPECT_FALSE(simulator.replay_replace_order(1, 2, Price{101}, Quantity{0}, Timestamp{2}));

    ASSERT_NE(simulator.order_book().find(1), nullptr);
    EXPECT_EQ(simulator.order_book().find(2), nullptr);
    EXPECT_EQ(simulator.orders_in_use(), 1u);
}

} // namespace
} // namespace simulator
