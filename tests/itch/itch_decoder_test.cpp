#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "simulator/itch/itch_decoder.hpp"
#include "simulator/itch/itch_normalizer.hpp"
#include "simulator/replay/replay_event.hpp"

namespace simulator {
namespace {

TEST(ItchDecoderTest, DecodesAddOrder)
{
    /*
     * ITCH Add Order:
     *
     * timestamp = 0x010203040506
     * order id  = 0x1122334455667788
     * side      = B
     * shares    = 100
     * stock     = "AAPL    "
     * price     = 12345
     */

    const std::array<std::byte, 31> payload{
        // timestamp - 6
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06},

        // order reference - 8
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33},
        std::byte{0x44},
        std::byte{0x55},
        std::byte{0x66},
        std::byte{0x77},
        std::byte{0x88},

        // side
        std::byte{'B'},

        // shares = 100
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x64},

        // stock = "AAPL    "
        std::byte{'A'},
        std::byte{'A'},
        std::byte{'P'},
        std::byte{'L'},
        std::byte{' '},
        std::byte{' '},
        std::byte{' '},
        std::byte{' '},

        // price = 12345
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x30},
        std::byte{0x39}
    };

    ItchMessage message;

    message.type = 'A';
    message.payload = payload;

    ItchDecoder decoder;

    ItchAddOrder order;

    EXPECT_EQ(
        decoder.decode_add_order(
            message,
            order
        ),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(
        order.timestamp,
        Timestamp{0x010203040506ULL}
    );

    EXPECT_EQ(
        order.order_id,
        OrderId{0x1122334455667788ULL}
    );

    EXPECT_EQ(
        order.side,
        Side::Buy
    );

    EXPECT_EQ(
        order.quantity,
        Quantity{100}
    );

    EXPECT_EQ(
        order.price,
        Price{12345}
    );
}

TEST(ItchDecoderTest, DecodesSellOrder)
{
    const std::array<std::byte, 31> payload{
        // timestamp
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x01},

        // order id
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x02},

        // side
        std::byte{'S'},

        // shares
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x64},

        // stock
        std::byte{'A'},
        std::byte{'A'},
        std::byte{'P'},
        std::byte{'L'},
        std::byte{' '},
        std::byte{' '},
        std::byte{' '},
        std::byte{' '},

        // price
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x27},
        std::byte{0x10}
    };

    ItchMessage message;

    message.type = 'A';
    message.payload = payload;

    ItchDecoder decoder;

    ItchAddOrder order;

    ASSERT_EQ(
        decoder.decode_add_order(
            message,
            order
        ),
        ItchDecodeResult::Success
    );

    EXPECT_EQ(order.side, Side::Sell);
    EXPECT_EQ(order.order_id, OrderId{2});
    EXPECT_EQ(order.quantity, Quantity{100});
    EXPECT_EQ(order.price, Price{10000});
}

TEST(ItchDecoderTest, RejectsWrongMessageType)
{
    const std::array<std::byte, 31> payload{};

    ItchMessage message;

    message.type = 'D';
    message.payload = payload;

    ItchDecoder decoder;

    ItchAddOrder order;

    EXPECT_EQ(
        decoder.decode_add_order(
            message,
            order
        ),
        ItchDecodeResult::UnsupportedMessage
    );

    EXPECT_EQ(
        decoder.error(),
        ItchDecodeError::InvalidMessageType
    );
}

TEST(ItchDecoderTest, RejectsWrongPayloadLength)
{
    const std::array<std::byte, 30> payload{};

    ItchMessage message;

    message.type = 'A';
    message.payload = payload;

    ItchDecoder decoder;

    ItchAddOrder order;

    EXPECT_EQ(
        decoder.decode_add_order(
            message,
            order
        ),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(
        decoder.error(),
        ItchDecodeError::InvalidPayloadLength
    );
}

TEST(ItchDecoderTest, RejectsInvalidSide)
{
    std::array<std::byte, 31> payload{};

    /*
     * Side is byte 14:
     *
     * timestamp = bytes 0-5
     * order id  = bytes 6-13
     * side      = byte 14
     */
    payload[14] = std::byte{'X'};

    ItchMessage message;

    message.type = 'A';
    message.payload = payload;

    ItchDecoder decoder;

    ItchAddOrder order;

    EXPECT_EQ(
        decoder.decode_add_order(
            message,
            order
        ),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(
        decoder.error(),
        ItchDecodeError::InvalidSide
    );
}

TEST(ItchDecoderTest, NormalizesAddOrder)
{
    ItchAddOrder order;

    order.timestamp = Timestamp{100};
    order.order_id = OrderId{123};
    order.side = Side::Buy;
    order.quantity = Quantity{500};
    order.price = Price{10100};

    ItchNormalizer normalizer;

    const ReplayEvent event =
        normalizer.normalize(order);

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
        OrderId{123}
    );

    EXPECT_EQ(
        event.side,
        Side::Buy
    );

    EXPECT_EQ(
        event.quantity,
        Quantity{500}
    );

    EXPECT_EQ(
        event.price,
        Price{10100}
    );
}

TEST(ItchDecoderTest, DecodesOrderExecuted)
{
    const std::array<std::byte, 26> payload{
        // Timestamp: 0x010203040506
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06},

        // Order ID: 0x1122334455667788
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33},
        std::byte{0x44},
        std::byte{0x55},
        std::byte{0x66},
        std::byte{0x77},
        std::byte{0x88},

        // Executed shares: 100
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x64},

        // Match number: 0x0102030405060708
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06},
        std::byte{0x07},
        std::byte{0x08}
    };

    ItchMessage message;

    message.type = 'E';
    message.payload = payload;

    ItchDecoder decoder;
    ItchOrderExecuted order;

    const auto result =
        decoder.decode_order_executed(
            message,
            order
        );

    ASSERT_EQ(
        result,
        ItchDecodeResult::Success
    );

    EXPECT_EQ(
        order.timestamp,
        0x010203040506ULL
    );

    EXPECT_EQ(
        order.order_id,
        0x1122334455667788ULL
    );

    EXPECT_EQ(
        order.quantity.value(),
        100
    );

    EXPECT_EQ(
        order.match_number,
        0x0102030405060708ULL
    );

    EXPECT_EQ(
        decoder.error(),
        ItchDecodeError::None
    );
}

TEST(ItchDecoderTest, DecodesOrderCancel)
{
    const std::array<std::byte, 18> payload{
        // Timestamp: 0x010203040506
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06},

        // Order ID: 0x1122334455667788
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33},
        std::byte{0x44},
        std::byte{0x55},
        std::byte{0x66},
        std::byte{0x77},
        std::byte{0x88},

        // Cancelled shares: 300
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x01},
        std::byte{0x2C}
    };

    ItchMessage message;

    message.type = 'X';
    message.payload = payload;

    ItchDecoder decoder;
    ItchOrderCancel order;

    const auto result =
        decoder.decode_order_cancel(
            message,
            order
        );

    ASSERT_EQ(
        result,
        ItchDecodeResult::Success
    );

    EXPECT_EQ(
        order.timestamp,
        0x010203040506ULL
    );

    EXPECT_EQ(
        order.order_id,
        0x1122334455667788ULL
    );

    EXPECT_EQ(
        order.quantity.value(),
        300
    );

    EXPECT_EQ(
        decoder.error(),
        ItchDecodeError::None
    );
}

TEST(ItchDecoderTest, DecodesOrderDelete)
{
    const std::array<std::byte, 14> payload{
        // Timestamp: 0x010203040506
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06},

        // Order ID: 0x1122334455667788
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33},
        std::byte{0x44},
        std::byte{0x55},
        std::byte{0x66},
        std::byte{0x77},
        std::byte{0x88}
    };

    ItchMessage message;

    message.type = 'D';
    message.payload = payload;

    ItchDecoder decoder;
    ItchOrderDelete order;

    const auto result =
        decoder.decode_order_delete(
            message,
            order
        );

    ASSERT_EQ(
        result,
        ItchDecodeResult::Success
    );

    EXPECT_EQ(
        order.timestamp,
        0x010203040506ULL
    );

    EXPECT_EQ(
        order.order_id,
        0x1122334455667788ULL
    );

    EXPECT_EQ(
        decoder.error(),
        ItchDecodeError::None
    );
}

TEST(ItchDecoderTest, OrderExecutedRejectsWrongMessageType)
{
    const std::array<std::byte, 26> payload{};

    ItchMessage message;

    message.type = 'A';
    message.payload = payload;

    ItchDecoder decoder;
    ItchOrderExecuted order;

    EXPECT_EQ(
        decoder.decode_order_executed(
            message,
            order
        ),
        ItchDecodeResult::UnsupportedMessage
    );

    EXPECT_EQ(
        decoder.error(),
        ItchDecodeError::InvalidMessageType
    );
}

TEST(ItchDecoderTest, OrderCancelRejectsWrongPayloadLength)
{
    const std::array<std::byte, 17> payload{};

    ItchMessage message;

    message.type = 'X';
    message.payload = payload;

    ItchDecoder decoder;
    ItchOrderCancel order;

    EXPECT_EQ(
        decoder.decode_order_cancel(
            message,
            order
        ),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(
        decoder.error(),
        ItchDecodeError::InvalidPayloadLength
    );
}

TEST(ItchDecoderTest, OrderDeleteRejectsWrongPayloadLength)
{
    const std::array<std::byte, 13> payload{};

    ItchMessage message;

    message.type = 'D';
    message.payload = payload;

    ItchDecoder decoder;
    ItchOrderDelete order;

    EXPECT_EQ(
        decoder.decode_order_delete(
            message,
            order
        ),
        ItchDecodeResult::InvalidMessage
    );

    EXPECT_EQ(
        decoder.error(),
        ItchDecodeError::InvalidPayloadLength
    );
}

} // namespace
} // namespace simulator