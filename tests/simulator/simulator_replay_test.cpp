#include <gtest/gtest.h>

#include "simulator/simulator.hpp"

namespace simulator {

TEST(
    SimulatorReplayTest,
    AppliesHistoricalOrderLifecycle
)
{
    Simulator simulator{16};

    ASSERT_TRUE(
        simulator.replay_add_order(
            100,
            Side::Buy,
            Price{100},
            Quantity{1000},
            Timestamp{100}
        )
    );

    const Order* order =
        simulator.order_book().find(100);

    ASSERT_NE(
        order,
        nullptr
    );

    EXPECT_EQ(
        order->remaining_quantity().value(),
        1000
    );

    EXPECT_EQ(
        order->state(),
        OrderState::Active
    );

    ASSERT_TRUE(
        simulator.replay_execute_order(
            100,
            Quantity{300}
        )
    );

    order =
        simulator.order_book().find(100);

    ASSERT_NE(
        order,
        nullptr
    );

    EXPECT_EQ(
        order->remaining_quantity().value(),
        700
    );

    EXPECT_EQ(
        order->state(),
        OrderState::PartiallyFilled
    );

    ASSERT_TRUE(
        simulator.replay_cancel_order(
            100,
            Quantity{200}
        )
    );

    order =
        simulator.order_book().find(100);

    ASSERT_NE(
        order,
        nullptr
    );

    EXPECT_EQ(
        order->remaining_quantity().value(),
        500
    );

    EXPECT_EQ(
        order->state(),
        OrderState::PartiallyFilled
    );

    ASSERT_TRUE(
        simulator.replay_delete_order(100)
    );

    EXPECT_EQ(
        simulator.order_book().find(100),
        nullptr
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        0
    );

    EXPECT_EQ(
        simulator.orders_available(),
        simulator.orders_in_use() + 16
    );
}

TEST(
    SimulatorReplayTest,
    ReleasesFullyExecutedOrder
)
{
    Simulator simulator{16};

    ASSERT_TRUE(
        simulator.replay_add_order(
            100,
            Side::Buy,
            Price{100},
            Quantity{100},
            Timestamp{100}
        )
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        1
    );

    ASSERT_TRUE(
        simulator.replay_execute_order(
            100,
            Quantity{100}
        )
    );

    EXPECT_EQ(
        simulator.order_book().find(100),
        nullptr
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        0
    );

    EXPECT_EQ(
        simulator.orders_available(),
        16
    );
}

} // namespace simulator