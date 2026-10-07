#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "simulator/replay/replay_engine.hpp"
#include "simulator/replay/csv_replay_event_reader.hpp"

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

    const ReplayResult result = replay.replay(events);

    ASSERT_TRUE(result.succeeded());

    EXPECT_EQ(result.statistics.received_events, 2);
    EXPECT_EQ(result.statistics.processed_events, 2);
    EXPECT_EQ(result.statistics.add_events, 2);
    EXPECT_EQ(result.statistics.rejected_events, 0);

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

    EXPECT_EQ(
        simulator.trades().size(),
        0
    );
}


TEST(ReplayEngineTest, DoesNotMatchHistoricalAddEvents)
{
    Simulator simulator{10};
    ReplayEngine replay{simulator};

    /*
     * These are historical exchange events.
     *
     * Even though the BUY crosses the SELL, ReplayEngine must
     * not run the matching engine. The historical feed tells us
     * exactly what happened through subsequent Execute messages.
     */
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

    const ReplayResult result = replay.replay(events);

    ASSERT_TRUE(result.succeeded());

    EXPECT_EQ(result.statistics.processed_events, 2);

    EXPECT_EQ(
        simulator.trades().size(),
        0
    );

    /*
     * Both orders remain because no Execute/Delete/Cancel event
     * was present in the historical stream.
     */
    EXPECT_NE(
        simulator.order_book().find(1),
        nullptr
    );

    EXPECT_NE(
        simulator.order_book().find(2),
        nullptr
    );
}


TEST(ReplayEngineTest, ReplaysExecution)
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
            ReplayEventType::Execute,
            1,
            Side::Sell,
            Price{100},
            Quantity{40}
        }
    };

    const ReplayResult result = replay.replay(events);

    ASSERT_TRUE(result.succeeded());

    EXPECT_EQ(result.statistics.received_events, 2);
    EXPECT_EQ(result.statistics.processed_events, 2);
    EXPECT_EQ(result.statistics.add_events, 1);
    EXPECT_EQ(result.statistics.execute_events, 1);
    EXPECT_EQ(result.statistics.rejected_events, 0);

    const Order* order =
        simulator.order_book().find(1);

    ASSERT_NE(order, nullptr);

    EXPECT_EQ(
        order->remaining_quantity(),
        Quantity{60}
    );

    EXPECT_EQ(
        order->state(),
        OrderState::PartiallyFilled
    );

    EXPECT_EQ(
        simulator.trades().size(),
        0
    );
}


TEST(ReplayEngineTest, ReplaysFullExecutionAndRemovesOrder)
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
            ReplayEventType::Execute,
            1,
            Side::Sell,
            Price{100},
            Quantity{100}
        }
    };

    const ReplayResult result = replay.replay(events);

    ASSERT_TRUE(result.succeeded());

    EXPECT_EQ(result.statistics.processed_events, 2);
    EXPECT_EQ(result.statistics.execute_events, 1);

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

    EXPECT_EQ(
        simulator.trades().size(),
        0
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

    const ReplayResult result = replay.replay(events);

    ASSERT_TRUE(result.succeeded());

    EXPECT_EQ(result.statistics.processed_events, 2);
    EXPECT_EQ(result.statistics.add_events, 1);
    EXPECT_EQ(result.statistics.cancel_events, 1);
    EXPECT_EQ(result.statistics.rejected_events, 0);

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


TEST(ReplayEngineTest, ReplaysPartialCancellation)
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
            Quantity{40}
        }
    };

    const ReplayResult result = replay.replay(events);

    ASSERT_TRUE(result.succeeded());

    const Order* order =
        simulator.order_book().find(1);

    ASSERT_NE(order, nullptr);

    EXPECT_EQ(
        order->remaining_quantity(),
        Quantity{60}
    );

    EXPECT_EQ(
        order->state(),
        OrderState::PartiallyFilled
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        1
    );
}


TEST(ReplayEngineTest, ReplaysDelete)
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
            ReplayEventType::Delete,
            1,
            Side::Buy,
            Price{100},
            Quantity{0}
        }
    };

    const ReplayResult result = replay.replay(events);

    ASSERT_TRUE(result.succeeded());

    EXPECT_EQ(result.statistics.processed_events, 2);
    EXPECT_EQ(result.statistics.add_events, 1);
    EXPECT_EQ(result.statistics.delete_events, 1);

    EXPECT_EQ(
        simulator.order_book().find(1),
        nullptr
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        0
    );
}


TEST(ReplayEngineTest, ReplaysEventsDirectlyFromCsv)
{
    Simulator simulator{10};

    ReplayEngine replay{
        simulator
    };

    CsvReplayEventReader reader{
        std::string{CHRONOS_TEST_DATA_DIR} +
        "/basic_replay.csv"
    };

    ASSERT_TRUE(reader.is_open());

    const ReplayResult result =
        replay.replay(reader);

    ASSERT_TRUE(result.succeeded());

    EXPECT_EQ(
        result.statistics.received_events,
        4
    );

    EXPECT_EQ(
        result.statistics.processed_events,
        4
    );

    EXPECT_EQ(
        result.statistics.add_events,
        3
    );

    EXPECT_EQ(
        result.statistics.cancel_events,
        1
    );

    EXPECT_EQ(
        result.statistics.rejected_events,
        0
    );

    /*
     * Historical replay does not create trades merely because
     * two Add events cross.
     */
    EXPECT_EQ(
        result.statistics.trades,
        0
    );

    EXPECT_EQ(
        simulator.trades().size(),
        0
    );

    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{400}
    );

    /*
     * Order 3 was added and subsequently cancelled.
     */
    EXPECT_EQ(
        simulator.order_book().find(3),
        nullptr
    );
}

TEST(ReplayEngineTest, RejectsOutOfOrderTimestamp)
{
    Simulator simulator{10};
    ReplayEngine replay{simulator};

    const std::vector<ReplayEvent> events{
        ReplayEvent{
            Timestamp{200},
            ReplayEventType::Add,
            1,
            Side::Buy,
            Price{100},
            Quantity{100}
        },

        ReplayEvent{
            Timestamp{100},
            ReplayEventType::Add,
            2,
            Side::Buy,
            Price{99},
            Quantity{100}
        }
    };

    const ReplayResult result =
        replay.replay(events);

    EXPECT_FALSE(result.succeeded());

    EXPECT_EQ(
        result.failure,
        ReplayFailure::OutOfOrderTimestamp
    );

    EXPECT_EQ(
        result.failed_event_index,
        2
    );

    EXPECT_EQ(
        result.statistics.received_events,
        2
    );

    EXPECT_EQ(
        result.statistics.processed_events,
        1
    );

    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{200}
    );

    EXPECT_NE(
        simulator.order_book().find(1),
        nullptr
    );

    EXPECT_EQ(
        simulator.order_book().find(2),
        nullptr
    );
}

TEST(ReplayEngineTest, ReportsMalformedCsvAsReaderError)
{
    Simulator simulator{10};
    ReplayEngine replay{simulator};

    CsvReplayEventReader reader{
        std::string{CHRONOS_TEST_DATA_DIR} +
        "/malformed_replay.csv"
    };

    ASSERT_TRUE(reader.is_open());

    const ReplayResult result =
        replay.replay(reader);

    EXPECT_FALSE(result.succeeded());

    EXPECT_EQ(
        result.failure,
        ReplayFailure::ReaderError
    );

    EXPECT_EQ(
        result.failed_event_index,
        2
    );

    EXPECT_EQ(
        result.failed_source_line,
        3
    );

    EXPECT_EQ(
        result.statistics.received_events,
        1
    );

    EXPECT_EQ(
        result.statistics.processed_events,
        1
    );

    EXPECT_FALSE(
        reader.error().empty()
    );
}

TEST(ReplayEngineTest, RejectsUnknownCancelAndContinues)
{
    Simulator simulator{10};
    ReplayEngine replay{simulator};

    const std::vector<ReplayEvent> events{
        ReplayEvent{
            Timestamp{100},
            ReplayEventType::Cancel,
            99,
            Side::Buy,
            Price{100},
            Quantity{100}
        },

        ReplayEvent{
            Timestamp{200},
            ReplayEventType::Add,
            1,
            Side::Buy,
            Price{100},
            Quantity{100}
        }
    };

    const ReplayResult result =
        replay.replay(events);

    ASSERT_TRUE(result.succeeded());

    EXPECT_EQ(
        result.statistics.received_events,
        2
    );

    EXPECT_EQ(
        result.statistics.processed_events,
        1
    );

    EXPECT_EQ(
        result.statistics.add_events,
        1
    );

    EXPECT_EQ(
        result.statistics.cancel_events,
        0
    );

    EXPECT_EQ(
        result.statistics.rejected_events,
        1
    );

    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{200}
    );

    EXPECT_NE(
        simulator.order_book().find(1),
        nullptr
    );
}


TEST(ReplayEngineTest, RejectsPoolExhaustionAndContinues)
{
    Simulator simulator{1};
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
            ReplayEventType::Add,
            2,
            Side::Buy,
            Price{99},
            Quantity{100}
        },

        ReplayEvent{
            Timestamp{300},
            ReplayEventType::Cancel,
            1,
            Side::Buy,
            Price{100},
            Quantity{100}
        }
    };

    const ReplayResult result =
        replay.replay(events);

    ASSERT_TRUE(result.succeeded());

    EXPECT_EQ(
        result.statistics.received_events,
        3
    );

    EXPECT_EQ(
        result.statistics.processed_events,
        2
    );

    EXPECT_EQ(
        result.statistics.add_events,
        1
    );

    EXPECT_EQ(
        result.statistics.cancel_events,
        1
    );

    EXPECT_EQ(
        result.statistics.rejected_events,
        1
    );

    EXPECT_EQ(
        result.statistics.trades,
        0
    );

    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{300}
    );

    EXPECT_EQ(
        simulator.order_book().find(1),
        nullptr
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        0
    );
}

} // namespace

} // namespace simulator