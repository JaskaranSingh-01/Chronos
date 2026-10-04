#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "simulator/itch/itch_message_reader.hpp"

namespace simulator {
namespace {

TEST(ItchMessageReaderTest, ReadsSingleMessage)
{
    /*
     * Length = 3
     *
     * Message:
     *     type    = 0x41 ('A')
     *     payload = 0x10 0x20
     *
     * Wire:
     *
     * 00 03 41 10 20
     */
    const std::array<std::byte, 5> data{
        std::byte{0x00},
        std::byte{0x03},

        std::byte{0x41},
        std::byte{0x10},
        std::byte{0x20}
    };

    ItchMessageReader reader{data};

    ItchMessage message;

    EXPECT_EQ(
        reader.next(message),
        ItchReadResult::Message
    );

    EXPECT_EQ(message.type, 0x41);
    ASSERT_EQ(message.payload.size(), 2);

    EXPECT_EQ(
        message.payload[0],
        std::byte{0x10}
    );

    EXPECT_EQ(
        message.payload[1],
        std::byte{0x20}
    );

    EXPECT_EQ(reader.position(), 5);
    EXPECT_EQ(reader.remaining(), 0);
}

TEST(ItchMessageReaderTest, ReadsMultipleMessages)
{
    /*
     * Message A:
     *
     * length = 2
     * type   = 'A'
     * data   = 0x11
     *
     * Message B:
     *
     * length = 3
     * type   = 'B'
     * data   = 0x22 0x33
     *
     * Wire:
     *
     * 00 02 41 11
     * 00 03 42 22 33
     */
    const std::array<std::byte, 9> data{
        std::byte{0x00},
        std::byte{0x02},
        std::byte{0x41},
        std::byte{0x11},

        std::byte{0x00},
        std::byte{0x03},
        std::byte{0x42},
        std::byte{0x22},
        std::byte{0x33}
    };

    ItchMessageReader reader{data};

    ItchMessage message;

    ASSERT_EQ(
        reader.next(message),
        ItchReadResult::Message
    );

    EXPECT_EQ(message.type, 0x41);
    ASSERT_EQ(message.payload.size(), 1);

    EXPECT_EQ(
        message.payload[0],
        std::byte{0x11}
    );

    ASSERT_EQ(
        reader.next(message),
        ItchReadResult::Message
    );

    EXPECT_EQ(message.type, 0x42);
    ASSERT_EQ(message.payload.size(), 2);

    EXPECT_EQ(
        message.payload[0],
        std::byte{0x22}
    );

    EXPECT_EQ(
        message.payload[1],
        std::byte{0x33}
    );

    EXPECT_EQ(
        reader.next(message),
        ItchReadResult::EndOfBuffer
    );
}

TEST(ItchMessageReaderTest, HandlesMessageWithEmptyPayload)
{
    /*
     * Length = 1
     * Type   = 'A'
     *
     * Wire:
     *
     * 00 01 41
     */
    const std::array<std::byte, 3> data{
        std::byte{0x00},
        std::byte{0x01},
        std::byte{0x41}
    };

    ItchMessageReader reader{data};

    ItchMessage message;

    ASSERT_EQ(
        reader.next(message),
        ItchReadResult::Message
    );

    EXPECT_EQ(message.type, 0x41);
    EXPECT_TRUE(message.payload.empty());
}

TEST(ItchMessageReaderTest, RejectsZeroLengthMessage)
{
    const std::array<std::byte, 2> data{
        std::byte{0x00},
        std::byte{0x00}
    };

    ItchMessageReader reader{data};

    ItchMessage message;

    EXPECT_EQ(
        reader.next(message),
        ItchReadResult::Error
    );

    EXPECT_EQ(
        reader.error(),
        ItchReadError::InvalidLength
    );

    /*
     * Reader must not advance past the invalid message.
     */
    EXPECT_EQ(reader.position(), 0);
}

TEST(ItchMessageReaderTest, RejectsMissingLength)
{
    const std::array<std::byte, 1> data{
        std::byte{0x00}
    };

    ItchMessageReader reader{data};

    ItchMessage message;

    EXPECT_EQ(
        reader.next(message),
        ItchReadResult::Error
    );

    EXPECT_EQ(
        reader.error(),
        ItchReadError::MissingLength
    );

    EXPECT_EQ(reader.position(), 0);
}

TEST(ItchMessageReaderTest, RejectsTruncatedMessage)
{
    /*
     * Declared length = 5
     *
     * Only 3 message bytes exist.
     */
    const std::array<std::byte, 5> data{
        std::byte{0x00},
        std::byte{0x05},

        std::byte{0x41},
        std::byte{0x10},
        std::byte{0x20}
    };

    ItchMessageReader reader{data};

    ItchMessage message;

    EXPECT_EQ(
        reader.next(message),
        ItchReadResult::Error
    );

    EXPECT_EQ(
        reader.error(),
        ItchReadError::TruncatedMessage
    );

    /*
     * No bytes should be consumed when parsing fails.
     */
    EXPECT_EQ(reader.position(), 0);
    EXPECT_EQ(reader.remaining(), 5);
}

TEST(ItchMessageReaderTest, PreservesReaderAfterTruncatedSecondMessage)
{
    /*
     * First message is valid.
     *
     * Second message claims 5 bytes but only
     * 2 message bytes remain.
     */
    const std::array<std::byte, 8> data{
        std::byte{0x00},
        std::byte{0x02},
        std::byte{0x41},
        std::byte{0x11},

        std::byte{0x00},
        std::byte{0x05},
        std::byte{0x42},
        std::byte{0x22}
    };

    ItchMessageReader reader{data};

    ItchMessage message;

    ASSERT_EQ(
        reader.next(message),
        ItchReadResult::Message
    );

    EXPECT_EQ(message.type, 0x41);

    EXPECT_EQ(reader.position(), 4);

    EXPECT_EQ(
        reader.next(message),
        ItchReadResult::Error
    );

    EXPECT_EQ(
        reader.error(),
        ItchReadError::TruncatedMessage
    );

    /*
     * Position remains at the beginning of the
     * malformed second message.
     */
    EXPECT_EQ(reader.position(), 4);
}

TEST(ItchMessageReaderTest, SupportsLargeMessageLength)
{
    /*
     * 0x0100 = 256 bytes including message type.
     */
    std::array<std::byte, 258> data{};

    data[0] = std::byte{0x01};
    data[1] = std::byte{0x00};

    data[2] = std::byte{0x41};

    ItchMessageReader reader{data};

    ItchMessage message;

    ASSERT_EQ(
        reader.next(message),
        ItchReadResult::Message
    );

    EXPECT_EQ(message.type, 0x41);
    EXPECT_EQ(message.payload.size(), 255);
    EXPECT_EQ(reader.position(), 258);
}

} // namespace
} // namespace simulator