#include <gtest/gtest.h>

#include <string>

#include "simulator/replay/csv_replay_event_reader.hpp"

namespace simulator {
namespace {

TEST(CsvReplayEventReaderTest, ReadsAddEvent)
{
    CsvReplayEventReader reader{
        std::string{CHRONOS_TEST_DATA_DIR} +
        "/basic_replay.csv"
    };

    ASSERT_TRUE(reader.is_open());

    ReplayEvent event;

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.timestamp,
        Timestamp{100}
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Add
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{1}
    );

    EXPECT_EQ(
        event.side,
        Side::Sell
    );

    EXPECT_EQ(
        event.price,
        Price{100}
    );

    EXPECT_EQ(
        event.quantity,
        Quantity{100}
    );
}

TEST(CsvReplayEventReaderTest, ReadsMultipleEvents)
{
    CsvReplayEventReader reader{
        std::string{CHRONOS_TEST_DATA_DIR} +
        "/basic_replay.csv"
    };

    ASSERT_TRUE(reader.is_open());

    ReplayEvent event;

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{1}
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{2}
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{3}
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Cancel
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{3}
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::EndOfFile
    );
}

TEST(CsvReplayEventReaderTest, RejectsMissingFile)
{
    CsvReplayEventReader reader{
        std::string{CHRONOS_TEST_DATA_DIR} +
        "/does_not_exist.csv"
    };

    EXPECT_FALSE(reader.is_open());

    ReplayEvent event;

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Error
    );

    EXPECT_FALSE(
        reader.error().empty()
    );
}

TEST(CsvReplayEventReaderTest, RejectsMalformedRow)
{
    CsvReplayEventReader reader{
        std::string{CHRONOS_TEST_DATA_DIR} +
        "/malformed_replay.csv"
    };

    ASSERT_TRUE(reader.is_open());

    ReplayEvent event;

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Error
    );

    EXPECT_EQ(
        reader.line_number(),
        3
    );

    EXPECT_FALSE(
        reader.error().empty()
    );
}

} // namespace
} // namespace simulator