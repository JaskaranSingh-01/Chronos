#include <gtest/gtest.h>

#include <vector>

#include "simulator/replay/replay_engine.hpp"

namespace simulator {
namespace {

TEST(ReplayEngineTest, ReplaysAddEvents)
{
    Simulator simulator{10};

    ReplayEngine replay{simulator};

    const std::vector<ReplayEvent> events{
        ReplayEvent{
            Timestamp{100},
            ReplayEventType::Add,
            1,
            Side::Buy,
            Price{100},
            Quantity{100}
        },
        ReplayEvent{
            Timestamp{110},
            ReplayEventType::Add,
            2,
            Side::Sell,
            Price{101},
            Quantity{50}
        }
    };

    const std::size_t processed =
        replay.replay(events);

    EXPECT_EQ(processed, 2);

    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{110}
    );

    EXPECT_NE(
        simulator.order_book().find(1),
        nullptr
    );

    EXPECT_NE(
        simulator.order_book().find(2),
        nullptr
    );
}

TEST(ReplayEngineTest, ReplaysOrdersThatMatch)
{
    Simulator simulator{10};

    ReplayEngine replay{simulator};

    const std::vector<ReplayEvent> events{
        ReplayEvent{
            Timestamp{100},
            ReplayEventType::Add,
            1,
            Side::Sell,
            Price{100},
            Quantity{100}
        },
        ReplayEvent{
            Timestamp{200},
            ReplayEventType::Add,
            2,
            Side::Buy,
            Price{101},
            Quantity{100}
        }
    };

    ASSERT_EQ(
        replay.replay(events),
        2
    );

    ASSERT_EQ(
        simulator.trades().size(),
        1
    );

    const Trade& trade =
        simulator.trades()[0];

    EXPECT_EQ(trade.buy_order_id, 2);
    EXPECT_EQ(trade.sell_order_id, 1);
    EXPECT_EQ(trade.price, Price{100});
    EXPECT_EQ(trade.quantity, Quantity{100});

    EXPECT_EQ(
        simulator.order_book().find(1),
        nullptr
    );

    EXPECT_EQ(
        simulator.order_book().find(2),
        nullptr
    );
}

TEST(ReplayEngineTest, ReplaysCancellation)
{
    Simulator simulator{10};

    ReplayEngine replay{simulator};

    const std::vector<ReplayEvent> events{
        ReplayEvent{
            Timestamp{100},
            ReplayEventType::Add,
            1,
            Side::Buy,
            Price{100},
            Quantity{100}
        },

        ReplayEvent{
            Timestamp{200},
            ReplayEventType::Cancel,
            1,
            Side::Buy,
            Price{100},
            Quantity{100}
        }
    };

    ASSERT_EQ(
        replay.replay(events),
        2
    );

    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{200}
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

TEST(ReplayEngineTest, ReplaysEventsDirectlyFromCsv)
{
    Simulator simulator{10};

    ReplayEngine replay{
        simulator
    };

    ReplayEventReader reader{
    std::string{CHRONOS_TEST_DATA_DIR} + "/basic_replay.csv"
};

    ASSERT_TRUE(reader.is_open());

    const std::size_t processed =
        replay.replay(reader);

    EXPECT_EQ(processed, 4);

    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{400}
    );

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

    // Order 3 was added and subsequently cancelled.
    EXPECT_EQ(
        simulator.order_book().find(3),
        nullptr
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        0
    );
}

} // namespace
} // namespace simulator