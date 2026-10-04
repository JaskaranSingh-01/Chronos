#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "simulator/itch/byte_reader.hpp"

namespace simulator {
namespace {

TEST(ByteReaderTest, ReadsU8)
{
    const std::array<std::byte, 2> data{
        std::byte{0x12},
        std::byte{0x34}
    };

    ByteReader reader{data};

    std::uint8_t value{0};

    ASSERT_TRUE(reader.read_u8(value));

    EXPECT_EQ(value, 0x12);
    EXPECT_EQ(reader.position(), 1);
    EXPECT_EQ(reader.remaining(), 1);
}

TEST(ByteReaderTest, ReadsU16BigEndian)
{
    const std::array<std::byte, 2> data{
        std::byte{0x12},
        std::byte{0x34}
    };

    ByteReader reader{data};

    std::uint16_t value{0};

    ASSERT_TRUE(reader.read_u16_be(value));

    EXPECT_EQ(value, 0x1234);
    EXPECT_EQ(reader.position(), 2);
    EXPECT_EQ(reader.remaining(), 0);
}

TEST(ByteReaderTest, ReadsU32BigEndian)
{
    const std::array<std::byte, 4> data{
        std::byte{0x12},
        std::byte{0x34},
        std::byte{0x56},
        std::byte{0x78}
    };

    ByteReader reader{data};

    std::uint32_t value{0};

    ASSERT_TRUE(reader.read_u32_be(value));

    EXPECT_EQ(value, 0x12345678U);
    EXPECT_EQ(reader.position(), 4);
    EXPECT_EQ(reader.remaining(), 0);
}

TEST(ByteReaderTest, ReadsU64BigEndian)
{
    const std::array<std::byte, 8> data{
        std::byte{0x01},
        std::byte{0x23},
        std::byte{0x45},
        std::byte{0x67},
        std::byte{0x89},
        std::byte{0xAB},
        std::byte{0xCD},
        std::byte{0xEF}
    };

    ByteReader reader{data};

    std::uint64_t value{0};

    ASSERT_TRUE(reader.read_u64_be(value));

    EXPECT_EQ(
        value,
        0x0123456789ABCDEFULL
    );

    EXPECT_EQ(reader.position(), 8);
    EXPECT_EQ(reader.remaining(), 0);
}

TEST(ByteReaderTest, ReadsRawBytes)
{
    const std::array<std::byte, 4> data{
        std::byte{0x10},
        std::byte{0x20},
        std::byte{0x30},
        std::byte{0x40}
    };

    ByteReader reader{data};

    std::span<const std::byte> bytes;

    ASSERT_TRUE(
        reader.read_bytes(2, bytes)
    );

    ASSERT_EQ(bytes.size(), 2);

    EXPECT_EQ(bytes[0], std::byte{0x10});
    EXPECT_EQ(bytes[1], std::byte{0x20});

    EXPECT_EQ(reader.position(), 2);
    EXPECT_EQ(reader.remaining(), 2);
}

TEST(ByteReaderTest, CanReadMultipleValuesSequentially)
{
    const std::array<std::byte, 7> data{
        std::byte{0xAA},

        std::byte{0x01},
        std::byte{0x02},

        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06}
    };

    ByteReader reader{data};

    std::uint8_t first{0};
    std::uint16_t second{0};
    std::uint32_t third{0};

    ASSERT_TRUE(reader.read_u8(first));
    ASSERT_TRUE(reader.read_u16_be(second));
    ASSERT_TRUE(reader.read_u32_be(third));

    EXPECT_EQ(first, 0xAA);
    EXPECT_EQ(second, 0x0102);
    EXPECT_EQ(third, 0x03040506U);

    EXPECT_TRUE(reader.empty());
    EXPECT_EQ(reader.remaining(), 0);
}

TEST(ByteReaderTest, RejectsTruncatedU16)
{
    const std::array<std::byte, 1> data{
        std::byte{0x12}
    };

    ByteReader reader{data};

    std::uint16_t value{0};

    EXPECT_FALSE(
        reader.read_u16_be(value)
    );

    EXPECT_EQ(reader.position(), 0);
    EXPECT_EQ(reader.remaining(), 1);
}

TEST(ByteReaderTest, RejectsTruncatedU32)
{
    const std::array<std::byte, 3> data{
        std::byte{0x12},
        std::byte{0x34},
        std::byte{0x56}
    };

    ByteReader reader{data};

    std::uint32_t value{0};

    EXPECT_FALSE(
        reader.read_u32_be(value)
    );

    EXPECT_EQ(reader.position(), 0);
    EXPECT_EQ(reader.remaining(), 3);
}

TEST(ByteReaderTest, RejectsTruncatedU64)
{
    const std::array<std::byte, 7> data{
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06},
        std::byte{0x07}
    };

    ByteReader reader{data};

    std::uint64_t value{0};

    EXPECT_FALSE(
        reader.read_u64_be(value)
    );

    EXPECT_EQ(reader.position(), 0);
    EXPECT_EQ(reader.remaining(), 7);
}

TEST(ByteReaderTest, RejectsTooManyBytes)
{
    const std::array<std::byte, 2> data{
        std::byte{0x10},
        std::byte{0x20}
    };

    ByteReader reader{data};

    std::span<const std::byte> bytes;

    EXPECT_FALSE(
        reader.read_bytes(3, bytes)
    );

    EXPECT_EQ(reader.position(), 0);
    EXPECT_EQ(reader.remaining(), 2);
}

TEST(ByteReaderTest, AllowsZeroByteRead)
{
    const std::array<std::byte, 2> data{
        std::byte{0x10},
        std::byte{0x20}
    };

    ByteReader reader{data};

    std::span<const std::byte> bytes;

    ASSERT_TRUE(
        reader.read_bytes(0, bytes)
    );

    EXPECT_TRUE(bytes.empty());
    EXPECT_EQ(reader.position(), 0);
    EXPECT_EQ(reader.remaining(), 2);
}

TEST(ByteReaderTest, ReadsU48BigEndian)
{
    const std::array<std::byte, 6> data{
        std::byte{0x01},
        std::byte{0x23},
        std::byte{0x45},
        std::byte{0x67},
        std::byte{0x89},
        std::byte{0xAB}
    };

    ByteReader reader{data};

    std::uint64_t value{0};

    ASSERT_TRUE(
        reader.read_u48_be(value)
    );

    EXPECT_EQ(
        value,
        0x0123456789ABULL
    );

    EXPECT_EQ(reader.position(), 6);
    EXPECT_EQ(reader.remaining(), 0);
}

TEST(ByteReaderTest, RejectsTruncatedU48)
{
    const std::array<std::byte, 5> data{
        std::byte{0x01},
        std::byte{0x23},
        std::byte{0x45},
        std::byte{0x67},
        std::byte{0x89}
    };

    ByteReader reader{data};

    std::uint64_t value{0};

    EXPECT_FALSE(
        reader.read_u48_be(value)
    );

    EXPECT_EQ(reader.position(), 0);
    EXPECT_EQ(reader.remaining(), 5);
}

TEST(ByteReaderTest, ReadsU48BetweenOtherFields)
{
    const std::array<std::byte, 9> data{
        // u8
        std::byte{0xAA},

        // u48
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
        std::byte{0x04},
        std::byte{0x05},
        std::byte{0x06},

        // u16
        std::byte{0x12},
        std::byte{0x34}
    };

    ByteReader reader{data};

    std::uint8_t first{0};
    std::uint64_t timestamp{0};
    std::uint16_t last{0};

    ASSERT_TRUE(reader.read_u8(first));
    ASSERT_TRUE(reader.read_u48_be(timestamp));
    ASSERT_TRUE(reader.read_u16_be(last));

    EXPECT_EQ(first, 0xAA);
    EXPECT_EQ(timestamp, 0x010203040506ULL);
    EXPECT_EQ(last, 0x1234);

    EXPECT_TRUE(reader.empty());
}

} // namespace
} // namespace simulator