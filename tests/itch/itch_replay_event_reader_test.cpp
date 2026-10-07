#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "simulator/itch/itch_replay_event_reader.hpp"

namespace simulator {
namespace {

void append_u16_be(
    std::vector<std::byte>& data,
    std::uint16_t value
)
{
    data.push_back(
        static_cast<std::byte>((value >> 8) & 0xff)
    );

    data.push_back(
        static_cast<std::byte>(value & 0xff)
    );
}

void append_u32_be(
    std::vector<std::byte>& data,
    std::uint32_t value
)
{
    data.push_back(
        static_cast<std::byte>((value >> 24) & 0xff)
    );

    data.push_back(
        static_cast<std::byte>((value >> 16) & 0xff)
    );

    data.push_back(
        static_cast<std::byte>((value >> 8) & 0xff)
    );

    data.push_back(
        static_cast<std::byte>(value & 0xff)
    );
}

void append_u64_be(
    std::vector<std::byte>& data,
    std::uint64_t value
)
{
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        data.push_back(
            static_cast<std::byte>(
                (value >> shift) & 0xff
            )
        );
    }
}

void append_u48_be(
    std::vector<std::byte>& data,
    std::uint64_t value
)
{
    for (int shift = 40; shift >= 0; shift -= 8)
    {
        data.push_back(
            static_cast<std::byte>(
                (value >> shift) & 0xff
            )
        );
    }
}

void append_bytes(
    std::vector<std::byte>& data,
    const char* value,
    std::size_t size
)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        data.push_back(
            static_cast<std::byte>(
                static_cast<unsigned char>(value[i])
            )
        );
    }
}

/*
 * NASDAQ ITCH Add Order message.
 *
 * Payload:
 *
 * Timestamp      6 bytes
 * Order ID       8 bytes
 * Side           1 byte
 * Shares         4 bytes
 * Stock          8 bytes
 * Price          4 bytes
 *
 * Payload = 31 bytes
 *
 * Message body includes:
 *
 * Message Type   1 byte
 * Payload       31 bytes
 *
 * Therefore the ITCH length field is 32.
 */
void append_add_order(
    std::vector<std::byte>& data,
    std::uint64_t timestamp,
    std::uint64_t order_id,
    char side,
    std::uint32_t shares,
    const char* stock,
    std::uint32_t price
)
{
    append_u16_be(data, 32);

    data.push_back(
        static_cast<std::byte>('A')
    );

    append_u48_be(data, timestamp);
    append_u64_be(data, order_id);

    data.push_back(
        static_cast<std::byte>(side)
    );

    append_u32_be(data, shares);

    append_bytes(data, stock, 8);

    append_u32_be(data, price);
}

/*
 * NASDAQ ITCH Order Executed message.
 *
 * Timestamp       6
 * Order ID        8
 * Executed Shares 4
 * Match Number    8
 *
 * Payload = 26 bytes
 *
 * Message body = 27 bytes including type.
 */
void append_order_executed(
    std::vector<std::byte>& data,
    std::uint64_t timestamp,
    std::uint64_t order_id,
    std::uint32_t shares,
    std::uint64_t match_number
)
{
    append_u16_be(data, 27);

    data.push_back(
        static_cast<std::byte>('E')
    );

    append_u48_be(data, timestamp);
    append_u64_be(data, order_id);
    append_u32_be(data, shares);
    append_u64_be(data, match_number);
}

/*
 * NASDAQ ITCH Order Cancel message.
 *
 * Timestamp  6
 * Order ID   8
 * Shares     4
 *
 * Payload = 18 bytes
 *
 * Message body = 19 bytes including type.
 */
void append_order_cancel(
    std::vector<std::byte>& data,
    std::uint64_t timestamp,
    std::uint64_t order_id,
    std::uint32_t shares
)
{
    append_u16_be(data, 19);

    data.push_back(
        static_cast<std::byte>('X')
    );

    append_u48_be(data, timestamp);
    append_u64_be(data, order_id);
    append_u32_be(data, shares);
}

/*
 * NASDAQ ITCH Order Delete message.
 *
 * Timestamp  6
 * Order ID   8
 *
 * Payload = 14 bytes
 *
 * Message body = 15 bytes including type.
 */
void append_order_delete(
    std::vector<std::byte>& data,
    std::uint64_t timestamp,
    std::uint64_t order_id
)
{
    append_u16_be(data, 15);

    data.push_back(
        static_cast<std::byte>('D')
    );

    append_u48_be(data, timestamp);
    append_u64_be(data, order_id);
}

void append_unsupported_message(
    std::vector<std::byte>& data,
    std::uint8_t type
)
{
    /*
     * Length = 1 because the message contains only
     * the message-type byte.
     */
    append_u16_be(data, 1);

    data.push_back(
        static_cast<std::byte>(type)
    );
}

} // namespace


TEST(
    ItchReplayReaderTest,
    ReadsAddOrder
)
{
    std::vector<std::byte> data;

    append_add_order(
        data,
        100,
        12345,
        'B',
        100,
        "AAPL    ",
        10100
    );

    ItchReplayReader reader{data};

    ReplayEvent event{};

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
        OrderId{12345}
    );

    EXPECT_EQ(
        event.side,
        Side::Buy
    );

    EXPECT_EQ(
        event.quantity.value(),
        100
    );

    EXPECT_EQ(
        event.price.ticks,
        10100
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::EndOfFile
    );
}


TEST(
    ItchReplayReaderTest,
    ReadsExecuteOrder
)
{
    std::vector<std::byte> data;

    append_order_executed(
        data,
        200,
        12345,
        50,
        987654
    );

    ItchReplayReader reader{data};

    ReplayEvent event{};

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.timestamp,
        Timestamp{200}
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Execute
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{12345}
    );

    EXPECT_EQ(
        event.quantity.value(),
        50
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::EndOfFile
    );
}


TEST(
    ItchReplayReaderTest,
    ReadsCancelOrder
)
{
    std::vector<std::byte> data;

    append_order_cancel(
        data,
        300,
        12345,
        25
    );

    ItchReplayReader reader{data};

    ReplayEvent event{};

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.timestamp,
        Timestamp{300}
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Cancel
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{12345}
    );

    EXPECT_EQ(
        event.quantity.value(),
        25
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::EndOfFile
    );
}


TEST(
    ItchReplayReaderTest,
    ReadsDeleteOrder
)
{
    std::vector<std::byte> data;

    append_order_delete(
        data,
        400,
        12345
    );

    ItchReplayReader reader{data};

    ReplayEvent event{};

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.timestamp,
        Timestamp{400}
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Delete
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{12345}
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::EndOfFile
    );
}


TEST(
    ItchReplayReaderTest,
    SkipsUnsupportedMessages
)
{
    std::vector<std::byte> data;

    append_unsupported_message(
        data,
        0x53
    );

    append_add_order(
        data,
        100,
        12345,
        'B',
        100,
        "AAPL    ",
        10100
    );

    ItchReplayReader reader{data};

    ReplayEvent event{};

    /*
     * The unsupported message must not be returned.
     * The reader should continue until it finds the
     * next supported ITCH message.
     */
    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Add
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{12345}
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::EndOfFile
    );
}


TEST(
    ItchReplayReaderTest,
    ReadsMultipleMessagesInOrder
)
{
    std::vector<std::byte> data;

    append_add_order(
        data,
        100,
        1,
        'S',
        100,
        "AAPL    ",
        10000
    );

    append_add_order(
        data,
        200,
        2,
        'B',
        100,
        "AAPL    ",
        10100
    );

    append_order_executed(
        data,
        300,
        1,
        100,
        1001
    );

    append_order_executed(
        data,
        400,
        2,
        100,
        1002
    );

    ItchReplayReader reader{data};

    ReplayEvent event{};

    ASSERT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Add
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{1}
    );

    ASSERT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Add
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{2}
    );

    ASSERT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Execute
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{1}
    );

    ASSERT_EQ(
        reader.next(event),
        ReplayReadResult::Event
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Execute
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{2}
    );

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::EndOfFile
    );
}


TEST(
    ItchReplayReaderTest,
    ReportsTruncatedMessage
)
{
    std::vector<std::byte> data;

    /*
     * Claim that 32 bytes follow, but provide only
     * the message type.
     */
    append_u16_be(data, 32);

    data.push_back(
        static_cast<std::byte>('A')
    );

    ItchReplayReader reader{data};

    ReplayEvent event{};

    EXPECT_EQ(
        reader.next(event),
        ReplayReadResult::Error
    );
}

} // namespace simulator