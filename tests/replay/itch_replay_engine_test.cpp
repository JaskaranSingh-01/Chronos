#include <gtest/gtest.h>

#include "../itch/itch_test_builder.hpp"
#include "simulator/itch/itch_replay_event_reader.hpp"
#include "simulator/replay/replay_engine.hpp"
#include "simulator/simulator.hpp"

namespace simulator {
using itch_test::ItchBuilder;


TEST(
    ItchReplayEngineTest,
    ReplaysHistoricalOrdersWithoutMatching
)
{
    ItchBuilder builder;

    builder
        /*
         * Historical order 1:
         *
         * SELL 100 @ 100.00
         */
        .add_order(1, 100, 1, 'S', 100, "AAPL", 10000)

        /*
         * Historical order 2:
         *
         * BUY 100 @ 101.00
         *
         * These prices cross:
         *
         * BUY  101
         * SELL 100
         *
         * However, these are exchange-generated historical
         * orders. They must NOT go through MatchingEngine.
         */
        .add_order(1, 200, 2, 'B', 100, "AAPL", 10100)

        /*
         * The exchange explicitly tells us that both
         * orders were executed.
         */
        .executed(1, 300, 1, 100, 1001)
        .executed(1, 400, 2, 100, 1002);

    Simulator simulator{16};

    ReplayEngine engine{simulator};

    ItchReplayReader reader{builder.bytes()};

    const ReplayResult result =
        engine.replay(reader);

    /*
     * Four normalized ReplayEvents were produced:
     *
     * A
     * A
     * E
     * E
     */
    EXPECT_EQ(
        result.statistics.received_events,
        4
    );

    EXPECT_EQ(
        result.statistics.processed_events,
        4
    );

    EXPECT_EQ(
        result.statistics.rejected_events,
        0
    );

    EXPECT_EQ(
        result.failure,
        ReplayFailure::None
    );

    /*
     * Both historical orders were fully executed
     * and therefore removed from the OrderBook.
     */
    EXPECT_TRUE(
        simulator.order_book().empty()
    );

    /*
     * Most important assertion:
     *
     * Historical replay must NOT generate simulated
     * trades through MatchingEngine.
     */
    EXPECT_TRUE(
        simulator.trades().empty()
    );

    /*
     * Filled historical orders should have been
     * returned to the pool.
     */
    EXPECT_EQ(
        simulator.orders_in_use(),
        0
    );

    EXPECT_EQ(
        simulator.orders_available(),
        16
    );

    /*
     * ReplayEngine should have advanced the simulator
     * clock to the final event timestamp.
     */
    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{400}
    );
}


TEST(
    ItchReplayEngineTest,
    ReplaysPartialExecution
)
{
    ItchBuilder builder;

    builder
        /*
         * Add 100 shares.
         */
        .add_order(1, 100, 1, 'S', 100, "AAPL", 10000)

        /*
         * Execute only 40 shares.
         */
        .executed(1, 200, 1, 40, 2001);

    Simulator simulator{16};

    ReplayEngine engine{simulator};

    ItchReplayReader reader{builder.bytes()};

    const ReplayResult result =
        engine.replay(reader);

    EXPECT_EQ(
        result.statistics.received_events,
        2
    );

    EXPECT_EQ(
        result.statistics.processed_events,
        2
    );

    EXPECT_EQ(
        result.statistics.rejected_events,
        0
    );

    EXPECT_EQ(
        result.failure,
        ReplayFailure::None
    );

    /*
     * The order is only partially executed, so it
     * must still exist in the historical book.
     */
    EXPECT_FALSE(
        simulator.order_book().empty()
    );

    const Order* order =
        simulator.order_book().find(1);

    ASSERT_NE(
        order,
        nullptr
    );

    EXPECT_EQ(
        order->remaining_quantity().value(),
        60
    );

    EXPECT_EQ(
        order->state(),
        OrderState::PartiallyFilled
    );

    /*
     * No simulated trade.
     */
    EXPECT_TRUE(
        simulator.trades().empty()
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        1
    );

    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{200}
    );
}

TEST(
    ItchReplayEngineTest,
    ReplaysReplace
)
{
    ItchBuilder builder;

    builder
        /*
         * Add SELL 100 @ 100.00 as order 1.
         */
        .add_order(1, 100, 1, 'S', 100, "AAPL", 10000)

        /*
         * Replace order 1 with order 2: SELL 80 @ 101.00.
         * The replacement keeps the original side.
         */
        .replace(1, 200, 1, 2, 80, 10100)

        /*
         * Execute 30 shares of the replacement.
         */
        .executed(1, 300, 2, 30, 3001);

    Simulator simulator{16};

    ReplayEngine engine{simulator};

    ItchReplayReader reader{builder.bytes()};

    const ReplayResult result =
        engine.replay(reader);

    EXPECT_EQ(result.failure, ReplayFailure::None);
    EXPECT_EQ(result.statistics.received_events, 3);
    EXPECT_EQ(result.statistics.processed_events, 3);
    EXPECT_EQ(result.statistics.replace_events, 1);
    EXPECT_EQ(result.statistics.rejected_events, 0);

    /*
     * The original order is gone; only the replacement rests.
     */
    EXPECT_EQ(simulator.order_book().find(1), nullptr);

    const Order* order =
        simulator.order_book().find(2);

    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->side(), Side::Sell);
    EXPECT_EQ(order->price(), Price{10100});
    EXPECT_EQ(order->remaining_quantity().value(), 50);
    EXPECT_EQ(order->state(), OrderState::PartiallyFilled);

    EXPECT_EQ(simulator.order_book().best_ask(), order);
    EXPECT_EQ(simulator.order_book().best_bid(), nullptr);

    EXPECT_TRUE(simulator.trades().empty());
    EXPECT_EQ(simulator.orders_in_use(), 1);
    EXPECT_EQ(simulator.clock().now(), Timestamp{300});
}

TEST(
    ItchReplayEngineTest,
    ReplaysMpidAddThenExecutedWithPrice
)
{
    ItchBuilder builder;

    builder
        /*
         * MPID-attributed add: BUY 100 @ 100.00.
         */
        .add_order_mpid(1, 100, 1, 'B', 100, "AAPL", 10000, "GSCO")

        /*
         * Execute 40 shares at a price other than the
         * resting price. Only the quantity reaches the book.
         */
        .executed_with_price(1, 200, 1, 40, 4001, true, 10050);

    Simulator simulator{16};

    ReplayEngine engine{simulator};

    ItchReplayReader reader{builder.bytes()};

    const ReplayResult result =
        engine.replay(reader);

    EXPECT_EQ(result.failure, ReplayFailure::None);
    EXPECT_EQ(result.statistics.received_events, 2);
    EXPECT_EQ(result.statistics.processed_events, 2);
    EXPECT_EQ(result.statistics.add_events, 1);
    EXPECT_EQ(result.statistics.execute_events, 1);
    EXPECT_EQ(result.statistics.rejected_events, 0);

    const Order* order =
        simulator.order_book().find(1);

    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->side(), Side::Buy);
    EXPECT_EQ(order->price(), Price{10000});
    EXPECT_EQ(order->remaining_quantity().value(), 60);
    EXPECT_EQ(order->state(), OrderState::PartiallyFilled);

    EXPECT_TRUE(simulator.trades().empty());
    EXPECT_EQ(simulator.clock().now(), Timestamp{200});
}

} // namespace simulator
