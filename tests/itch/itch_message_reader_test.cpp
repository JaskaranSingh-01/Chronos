#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "simulator/itch/itch_message_reader.hpp"

namespace simulator {
namespace {

TEST(ItchMessageReaderTest, ReadsSingleMessage)
{
    // Length 3: type 'A', payload 10 20.
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
    // 'A' with payload 11, then 'B' with payload 22 33.
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
    // Length 1: the type byte alone.
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
    // Declares 5 message bytes; only 3 follow.
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

    EXPECT_EQ(reader.position(), 0);
    EXPECT_EQ(reader.remaining(), 5);
}

TEST(ItchMessageReaderTest, PreservesReaderAfterTruncatedSecondMessage)
{
    // A valid message, then one that declares 5 bytes with only 2 left.
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

    EXPECT_EQ(reader.position(), 4);
}

TEST(ItchMessageReaderTest, SupportsLargeMessageLength)
{
    // Length 0x0100: the type byte plus 255 payload bytes.
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