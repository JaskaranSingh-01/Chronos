#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "itch_golden_messages.hpp"
#include "itch_test_builder.hpp"
#include "simulator/itch/itch_decoder.hpp"
#include "simulator/itch/itch_message_reader.hpp"

namespace simulator {
namespace {

using itch_test::ItchBuilder;

// Reads the single framed message in `framed` through the real framing reader.
ItchMessage read_one(std::span<const std::byte> framed)
{
    ItchMessageReader reader{framed};
    ItchMessage message{};

    EXPECT_EQ(reader.next(message), ItchReadResult::Message);
    EXPECT_EQ(reader.remaining(), 0u);

    return message;
}

// Copy of a single framed message, one byte shorter (delta -1) or longer
// (delta +1), with the length prefix fixed so only the decoder sees the
// wrong size.
std::vector<std::byte> resized(std::span<const std::byte> framed, int delta)
{
    std::vector<std::byte> bytes(framed.begin(), framed.end());

    if (delta < 0) {
        bytes.pop_back();
    } else {
        bytes.push_back(std::byte{0});
    }

    const std::size_t length = bytes.size() - 2;
    bytes[0] = static_cast<std::byte>((length >> 8) & 0xff);
    bytes[1] = static_cast<std::byte>(length & 0xff);

    return bytes;
}

// A golden array is one message without its length prefix: [type][payload].
ItchMessage golden(std::span<const std::uint8_t> real)
{
    return ItchMessage{real[0], std::as_bytes(real).subspan(1)};
}

TEST(ItchDecoderTest, DecodesAddOrder)
{
    ItchBuilder builder;
    builder.add_order(7, 0x010203040506, 0x1122334455667788, 'B', 100, "AAPL", 12345);

    ItchDecoder decoder;
    ItchAddOrder order;

    ASSERT_EQ(
        decoder.decode_add_order(read_one(builder.bytes()), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::None);
    EXPECT_EQ(order.timestamp, 0x010203040506);
    EXPECT_EQ(order.order_id, OrderId{0x1122334455667788});
    EXPECT_EQ(order.side, Side::Buy);
    EXPECT_EQ(order.quantity, Quantity{100});
    EXPECT_EQ(order.price, Price{12345});
    EXPECT_EQ(order.stock_locate, 7);
}

TEST(ItchDecoderTest, DecodesSellOrder)
{
    ItchBuilder builder;
    builder.add_order(7, 1, 2, 'S', 100, "AAPL", 10000);

    ItchDecoder decoder;
    ItchAddOrder order;

    ASSERT_EQ(
        decoder.decode_add_order(read_one(builder.bytes()), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.side, Side::Sell);
    EXPECT_EQ(order.order_id, OrderId{2});
    EXPECT_EQ(order.quantity, Quantity{100});
    EXPECT_EQ(order.price, Price{10000});
}

TEST(ItchDecoderTest, RejectsWrongMessageType)
{
    ItchBuilder builder;
    builder.erase(7, 1, 2);

    ItchDecoder decoder;
    ItchAddOrder order;

    EXPECT_EQ(
        decoder.decode_add_order(read_one(builder.bytes()), order),
        ItchDecodeResult::UnsupportedMessage
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::InvalidMessageType);
}

TEST(ItchDecoderTest, RejectsWrongPayloadLength)
{
    ItchBuilder builder;
    builder.add_order(7, 1, 2, 'B', 100, "AAPL", 10000);

    const std::vector<std::byte> bytes = resized(builder.bytes(), -1);

    ItchDecoder decoder;
    ItchAddOrder order;

    EXPECT_EQ(
        decoder.decode_add_order(read_one(bytes), order),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::InvalidPayloadLength);
}

TEST(ItchDecoderTest, RejectsInvalidSide)
{
    ItchBuilder builder;
    builder.add_order(7, 1, 2, 'X', 100, "AAPL", 10000);

    ItchDecoder decoder;
    ItchAddOrder order;

    EXPECT_EQ(
        decoder.decode_add_order(read_one(builder.bytes()), order),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::InvalidSide);
}

TEST(ItchDecoderTest, DecodesOrderExecuted)
{
    ItchBuilder builder;
    builder.executed(7, 0x010203040506, 0x1122334455667788, 100, 0x0102030405060708);

    ItchDecoder decoder;
    ItchOrderExecuted order;

    ASSERT_EQ(
        decoder.decode_order_executed(read_one(builder.bytes()), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.timestamp, 0x010203040506);
    EXPECT_EQ(order.order_id, 0x1122334455667788ULL);
    EXPECT_EQ(order.quantity.value(), 100);
    EXPECT_EQ(order.match_number, 0x0102030405060708ULL);
    EXPECT_EQ(order.stock_locate, 7);
    EXPECT_EQ(decoder.error(), ItchDecodeError::None);
}

TEST(ItchDecoderTest, DecodesOrderCancel)
{
    ItchBuilder builder;
    builder.cancel(7, 0x010203040506, 0x1122334455667788, 300);

    ItchDecoder decoder;
    ItchOrderCancel order;

    ASSERT_EQ(
        decoder.decode_order_cancel(read_one(builder.bytes()), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.timestamp, 0x010203040506);
    EXPECT_EQ(order.order_id, 0x1122334455667788ULL);
    EXPECT_EQ(order.quantity.value(), 300);
    EXPECT_EQ(order.stock_locate, 7);
    EXPECT_EQ(decoder.error(), ItchDecodeError::None);
}

TEST(ItchDecoderTest, DecodesOrderDelete)
{
    ItchBuilder builder;
    builder.erase(7, 0x010203040506, 0x1122334455667788);

    ItchDecoder decoder;
    ItchOrderDelete order;

    ASSERT_EQ(
        decoder.decode_order_delete(read_one(builder.bytes()), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.timestamp, 0x010203040506);
    EXPECT_EQ(order.order_id, 0x1122334455667788ULL);
    EXPECT_EQ(order.stock_locate, 7);
    EXPECT_EQ(decoder.error(), ItchDecodeError::None);
}

TEST(ItchDecoderTest, OrderExecutedRejectsWrongMessageType)
{
    ItchBuilder builder;
    builder.add_order(7, 1, 2, 'B', 100, "AAPL", 10000);

    ItchDecoder decoder;
    ItchOrderExecuted order;

    EXPECT_EQ(
        decoder.decode_order_executed(read_one(builder.bytes()), order),
        ItchDecodeResult::UnsupportedMessage
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::InvalidMessageType);
}

TEST(ItchDecoderTest, OrderCancelRejectsWrongPayloadLength)
{
    ItchBuilder builder;
    builder.cancel(7, 1, 2, 300);

    const std::vector<std::byte> bytes = resized(builder.bytes(), -1);

    ItchDecoder decoder;
    ItchOrderCancel order;

    EXPECT_EQ(
        decoder.decode_order_cancel(read_one(bytes), order),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::InvalidPayloadLength);
}

TEST(ItchDecoderTest, OrderDeleteRejectsWrongPayloadLength)
{
    ItchBuilder builder;
    builder.erase(7, 1, 2);

    const std::vector<std::byte> bytes = resized(builder.bytes(), -1);

    ItchDecoder decoder;
    ItchOrderDelete order;

    EXPECT_EQ(
        decoder.decode_order_delete(read_one(bytes), order),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::InvalidPayloadLength);
}

TEST(ItchDecoderTest, DecodesOrderReplace)
{
    ItchBuilder builder;
    builder.replace(7, 0x010203040506, 0x1122334455667788, 0x2233445566778899, 300, 10100);

    ItchDecoder decoder;
    ItchOrderReplace order;

    ASSERT_EQ(
        decoder.decode_order_replace(read_one(builder.bytes()), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.timestamp, 0x010203040506);
    EXPECT_EQ(order.original_order_id, 0x1122334455667788ULL);
    EXPECT_EQ(order.replacement_order_id, 0x2233445566778899ULL);
    EXPECT_EQ(order.quantity.value(), 300);
    EXPECT_EQ(order.price, Price{10100});
    EXPECT_EQ(order.stock_locate, 7);
    EXPECT_EQ(decoder.error(), ItchDecodeError::None);
}

TEST(ItchDecoderTest, RejectsReplaceWrongLength)
{
    ItchBuilder builder;
    builder.replace(7, 1, 2, 3, 300, 10100);

    const std::vector<std::byte> bytes = resized(builder.bytes(), +1);

    ItchDecoder decoder;
    ItchOrderReplace order;

    EXPECT_EQ(
        decoder.decode_order_replace(read_one(bytes), order),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::InvalidPayloadLength);
}

TEST(ItchDecoderTest, DecodesAddOrderMpid)
{
    ItchBuilder builder;
    builder.add_order_mpid(7, 0x010203040506, 0x1122334455667788, 'S', 100, "AAPL", 12345, "GSCO");

    ItchDecoder decoder;
    ItchAddOrder order;

    ASSERT_EQ(
        decoder.decode_add_order_mpid(read_one(builder.bytes()), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::None);
    EXPECT_EQ(order.timestamp, 0x010203040506);
    EXPECT_EQ(order.order_id, OrderId{0x1122334455667788});
    EXPECT_EQ(order.side, Side::Sell);
    EXPECT_EQ(order.quantity, Quantity{100});
    EXPECT_EQ(order.price, Price{12345});
    EXPECT_EQ(order.stock_locate, 7);
}

TEST(ItchDecoderTest, DecodesOrderExecutedWithPrice)
{
    ItchBuilder builder;
    builder.executed_with_price(7, 0x010203040506, 0x1122334455667788, 100, 0x0102030405060708, true, 12345);

    ItchDecoder decoder;
    ItchOrderExecutedWithPrice order;

    ASSERT_EQ(
        decoder.decode_order_executed_with_price(read_one(builder.bytes()), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::None);
    EXPECT_EQ(order.timestamp, 0x010203040506);
    EXPECT_EQ(order.order_id, 0x1122334455667788ULL);
    EXPECT_EQ(order.quantity.value(), 100);
    EXPECT_EQ(order.match_number, 0x0102030405060708ULL);
    EXPECT_TRUE(order.printable);
    EXPECT_EQ(order.price, Price{12345});
    EXPECT_EQ(order.stock_locate, 7);
}

TEST(ItchDecoderTest, RejectsInvalidPrintableFlag)
{
    ItchBuilder builder;
    builder.executed_with_price(7, 1, 2, 100, 3, true, 12345);

    // The printable flag's framed offset: length 2 + type 1 + header 10
    // + order ref 8 + shares 4 + match 8.
    std::vector<std::byte> bytes(builder.bytes().begin(), builder.bytes().end());
    bytes[33] = std::byte{'X'};

    ItchDecoder decoder;
    ItchOrderExecutedWithPrice order;

    EXPECT_EQ(
        decoder.decode_order_executed_with_price(read_one(bytes), order),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::InvalidPrintable);
}

TEST(ItchDecoderTest, DecodesStockDirectory)
{
    ItchBuilder builder;
    builder.stock_directory(7, 0x010203040506, "AAPL");

    ItchDecoder decoder;
    ItchStockDirectory directory;

    ASSERT_EQ(
        decoder.decode_stock_directory(read_one(builder.bytes()), directory),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(decoder.error(), ItchDecodeError::None);
    EXPECT_EQ(directory.timestamp, 0x010203040506);
    EXPECT_EQ(directory.stock_locate, 7);
    EXPECT_EQ(std::string_view(directory.stock.data(), directory.stock.size()), "AAPL    ");
}

// Golden messages: the first real message of each type from the NASDAQ
// sample, decoded and checked against the values in its comment.

TEST(ItchDecoderTest, DecodesRealAddOrder)
{
    ItchDecoder decoder;
    ItchAddOrder order;

    ASSERT_EQ(
        decoder.decode_add_order(golden(itch_test::real_a), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.stock_locate, 496);
    EXPECT_EQ(order.timestamp, 14400000724386);
    EXPECT_EQ(order.order_id, 8881u);
    EXPECT_EQ(order.side, Side::Buy);
    EXPECT_EQ(order.quantity.value(), 2000);
    EXPECT_EQ(order.price, Price{1605800});
}

TEST(ItchDecoderTest, DecodesRealAddOrderMpid)
{
    ItchDecoder decoder;
    ItchAddOrder order;

    ASSERT_EQ(
        decoder.decode_add_order_mpid(golden(itch_test::real_f), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.stock_locate, 8899);
    EXPECT_EQ(order.timestamp, 14400063421370);
    EXPECT_EQ(order.order_id, 15732u);
    EXPECT_EQ(order.side, Side::Sell);
    EXPECT_EQ(order.quantity.value(), 100);
    EXPECT_EQ(order.price, Price{15300000});
}

TEST(ItchDecoderTest, DecodesRealOrderExecuted)
{
    ItchDecoder decoder;
    ItchOrderExecuted order;

    ASSERT_EQ(
        decoder.decode_order_executed(golden(itch_test::real_e), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.stock_locate, 6966);
    EXPECT_EQ(order.timestamp, 14400303314576);
    EXPECT_EQ(order.order_id, 39411u);
    EXPECT_EQ(order.quantity.value(), 3);
    EXPECT_EQ(order.match_number, 17791u);
}

TEST(ItchDecoderTest, DecodesRealOrderExecutedWithPrice)
{
    ItchDecoder decoder;
    ItchOrderExecutedWithPrice order;

    ASSERT_EQ(
        decoder.decode_order_executed_with_price(golden(itch_test::real_c), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.stock_locate, 7603);
    EXPECT_EQ(order.timestamp, 34086523596953);
    EXPECT_EQ(order.order_id, 9489356u);
    EXPECT_EQ(order.quantity.value(), 100);
    EXPECT_EQ(order.match_number, 50460u);
    EXPECT_FALSE(order.printable);
    EXPECT_EQ(order.price, Price{41800});
}

TEST(ItchDecoderTest, DecodesRealOrderCancel)
{
    ItchDecoder decoder;
    ItchOrderCancel order;

    ASSERT_EQ(
        decoder.decode_order_cancel(golden(itch_test::real_x), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.stock_locate, 5301);
    EXPECT_EQ(order.timestamp, 14400033107916);
    EXPECT_EQ(order.order_id, 8499u);
    EXPECT_EQ(order.quantity.value(), 500);
}

TEST(ItchDecoderTest, DecodesRealOrderDelete)
{
    ItchDecoder decoder;
    ItchOrderDelete order;

    ASSERT_EQ(
        decoder.decode_order_delete(golden(itch_test::real_d), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.stock_locate, 6644);
    EXPECT_EQ(order.timestamp, 14400019074978);
    EXPECT_EQ(order.order_id, 983u);
}

TEST(ItchDecoderTest, DecodesRealOrderReplace)
{
    ItchDecoder decoder;
    ItchOrderReplace order;

    ASSERT_EQ(
        decoder.decode_order_replace(golden(itch_test::real_u), order),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.stock_locate, 3816);
    EXPECT_EQ(order.timestamp, 14400031359987);
    EXPECT_EQ(order.original_order_id, 70u);
    EXPECT_EQ(order.replacement_order_id, 13502u);
    EXPECT_EQ(order.quantity.value(), 500);
    EXPECT_EQ(order.price, Price{391400});
}

TEST(ItchDecoderTest, DecodesRealStockDirectory)
{
    ItchDecoder decoder;
    ItchStockDirectory directory;

    ASSERT_EQ(
        decoder.decode_stock_directory(golden(itch_test::real_r), directory),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(directory.stock_locate, 1);
    EXPECT_EQ(directory.timestamp, 11354325413976);
    EXPECT_EQ(std::string_view(directory.stock.data(), directory.stock.size()), "A       ");
}

} // namespace
} // namespace simulator
