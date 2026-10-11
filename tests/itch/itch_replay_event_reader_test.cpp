#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "itch_test_builder.hpp"
#include "simulator/itch/itch_replay_event_reader.hpp"

namespace simulator {
namespace {

using itch_test::ItchBuilder;

TEST(ItchReplayReaderTest, ReadsAddOrder)
{
    ItchBuilder builder;
    builder.add_order(1, 100, 12345, 'B', 100, "AAPL", 10100);

    ItchReplayReader reader{builder.bytes()};

    ReplayEvent event{};

    EXPECT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.timestamp, Timestamp{100});
    EXPECT_EQ(event.type, ReplayEventType::Add);
    EXPECT_EQ(event.order_id, OrderId{12345});
    EXPECT_EQ(event.side, Side::Buy);
    EXPECT_EQ(event.quantity.value(), 100);
    EXPECT_EQ(event.price.ticks, 10100);

    EXPECT_EQ(reader.next(event), ReplayReadResult::EndOfFile);
}

TEST(ItchReplayReaderTest, ReadsExecuteOrder)
{
    ItchBuilder builder;
    builder.executed(1, 200, 12345, 50, 987654);

    ItchReplayReader reader{builder.bytes()};

    ReplayEvent event{};

    EXPECT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.timestamp, Timestamp{200});
    EXPECT_EQ(event.type, ReplayEventType::Execute);
    EXPECT_EQ(event.order_id, OrderId{12345});
    EXPECT_EQ(event.quantity.value(), 50);

    EXPECT_EQ(reader.next(event), ReplayReadResult::EndOfFile);
}

TEST(ItchReplayReaderTest, ReadsCancelOrder)
{
    ItchBuilder builder;
    builder.cancel(1, 300, 12345, 25);

    ItchReplayReader reader{builder.bytes()};

    ReplayEvent event{};

    EXPECT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.timestamp, Timestamp{300});
    EXPECT_EQ(event.type, ReplayEventType::Cancel);
    EXPECT_EQ(event.order_id, OrderId{12345});
    EXPECT_EQ(event.quantity.value(), 25);

    EXPECT_EQ(reader.next(event), ReplayReadResult::EndOfFile);
}

TEST(ItchReplayReaderTest, ReadsDeleteOrder)
{
    ItchBuilder builder;
    builder.erase(1, 400, 12345);

    ItchReplayReader reader{builder.bytes()};

    ReplayEvent event{};

    EXPECT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.timestamp, Timestamp{400});
    EXPECT_EQ(event.type, ReplayEventType::Delete);
    EXPECT_EQ(event.order_id, OrderId{12345});

    EXPECT_EQ(reader.next(event), ReplayReadResult::EndOfFile);
}

TEST(ItchReplayReaderTest, ReadsAddOrderMpid)
{
    ItchBuilder builder;
    builder.add_order_mpid(1, 100, 12345, 'S', 100, "AAPL", 10100, "GSCO");

    ItchReplayReader reader{builder.bytes()};

    ReplayEvent event{};

    EXPECT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.timestamp, Timestamp{100});
    EXPECT_EQ(event.type, ReplayEventType::Add);
    EXPECT_EQ(event.order_id, OrderId{12345});
    EXPECT_EQ(event.side, Side::Sell);
    EXPECT_EQ(event.quantity.value(), 100);
    EXPECT_EQ(event.price.ticks, 10100);

    EXPECT_EQ(reader.next(event), ReplayReadResult::EndOfFile);
}

TEST(ItchReplayReaderTest, ReadsExecutedWithPrice)
{
    ItchBuilder builder;
    builder.executed_with_price(1, 200, 12345, 50, 987654, true, 10200);

    ItchReplayReader reader{builder.bytes()};

    ReplayEvent event{};

    EXPECT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.timestamp, Timestamp{200});
    EXPECT_EQ(event.type, ReplayEventType::Execute);
    EXPECT_EQ(event.order_id, OrderId{12345});
    EXPECT_EQ(event.quantity.value(), 50);

    EXPECT_EQ(reader.next(event), ReplayReadResult::EndOfFile);
}

TEST(ItchReplayReaderTest, SkipsUnsupportedMessages)
{
    ItchBuilder builder;
    builder
        .system_event(0, 50, 'O')
        .add_order(1, 100, 12345, 'B', 100, "AAPL", 10100);

    ItchReplayReader reader{builder.bytes()};

    ReplayEvent event{};

    /*
     * The unsupported message must not be returned.
     * The reader should continue until it finds the
     * next supported ITCH message.
     */
    EXPECT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.type, ReplayEventType::Add);
    EXPECT_EQ(event.order_id, OrderId{12345});

    EXPECT_EQ(reader.next(event), ReplayReadResult::EndOfFile);
}

TEST(ItchReplayReaderTest, SkipsStockDirectory)
{
    ItchBuilder builder;
    builder
        .stock_directory(1, 50, "AAPL")
        .add_order(1, 100, 12345, 'B', 100, "AAPL", 10100);

    ItchReplayReader reader{builder.bytes()};

    ReplayEvent event{};

    /*
     * 'R' carries no order-book event; the reader skips it.
     */
    EXPECT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.type, ReplayEventType::Add);
    EXPECT_EQ(event.order_id, OrderId{12345});

    EXPECT_EQ(reader.next(event), ReplayReadResult::EndOfFile);
}

TEST(ItchReplayReaderTest, ReadsMultipleMessagesInOrder)
{
    ItchBuilder builder;
    builder
        .add_order(1, 100, 1, 'S', 100, "AAPL", 10000)
        .add_order(1, 200, 2, 'B', 100, "AAPL", 10100)
        .executed(1, 300, 1, 100, 1001)
        .executed(1, 400, 2, 100, 1002);

    ItchReplayReader reader{builder.bytes()};

    ReplayEvent event{};

    ASSERT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.type, ReplayEventType::Add);
    EXPECT_EQ(event.order_id, OrderId{1});

    ASSERT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.type, ReplayEventType::Add);
    EXPECT_EQ(event.order_id, OrderId{2});

    ASSERT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.type, ReplayEventType::Execute);
    EXPECT_EQ(event.order_id, OrderId{1});

    ASSERT_EQ(reader.next(event), ReplayReadResult::Event);
    EXPECT_EQ(event.type, ReplayEventType::Execute);
    EXPECT_EQ(event.order_id, OrderId{2});

    EXPECT_EQ(reader.next(event), ReplayReadResult::EndOfFile);
}

TEST(ItchReplayReaderTest, ReportsTruncatedMessage)
{
    ItchBuilder builder;
    builder.add_order(1, 100, 12345, 'B', 100, "AAPL", 10100);

    /*
     * Keep the length prefix and the message type only, so the
     * prefix claims more bytes than the buffer holds.
     */
    const std::vector<std::byte> data(builder.bytes().begin(), builder.bytes().begin() + 3);

    ItchReplayReader reader{data};

    ReplayEvent event{};

    EXPECT_EQ(reader.next(event), ReplayReadResult::Error);
}

} // namespace
} // namespace simulator
