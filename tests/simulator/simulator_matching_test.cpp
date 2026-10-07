#include <gtest/gtest.h>

#include "simulator/simulator.hpp"

namespace simulator {

TEST(SimulatorTest, SubmitOrdersThatMatch)
{
    Simulator simulator{10};

    Order* sell = simulator.submit_order(
        OrderId{1},
        Side::Sell,
        Price{100},
        Quantity{100},
        Timestamp{100}
    );

    ASSERT_NE(sell, nullptr);

    Order* buy = simulator.submit_order(
        OrderId{2},
        Side::Buy,
        Price{101},
        Quantity{100},
        Timestamp{200}
    );

    ASSERT_NE(buy, nullptr);

    ASSERT_EQ(
        simulator.trades().size(),
        1
    );

    const Trade& trade =
        simulator.trades()[0];

    EXPECT_EQ(
        trade.buy_order_id,
        OrderId{2}
    );

    EXPECT_EQ(
        trade.sell_order_id,
        OrderId{1}
    );

    EXPECT_EQ(
        trade.price,
        Price{100}
    );

    EXPECT_EQ(
        trade.quantity,
        Quantity{100}
    );

    EXPECT_EQ(
        simulator.order_book().find(1),
        nullptr
    );

    EXPECT_EQ(
        simulator.order_book().find(2),
        nullptr
    );
}

} // namespace simulator