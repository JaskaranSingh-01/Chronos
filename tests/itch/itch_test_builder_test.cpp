#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "itch_test_builder.hpp"

namespace simulator::itch_test {
namespace {

// Framed offsets: [u16 length][type][locate u16][tracking u16][timestamp u48][body...]
constexpr std::size_t type_at = 2;
constexpr std::size_t locate_at = 3;
constexpr std::size_t tracking_at = 5;
constexpr std::size_t timestamp_at = 7;
constexpr std::size_t body_at = 13;

std::uint64_t read_be(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::size_t width
)
{
    std::uint64_t value = 0;

    for (std::size_t i = 0; i < width; ++i)
    {
        value = (value << 8) | std::to_integer<std::uint64_t>(bytes[offset + i]);
    }

    return value;
}

char read_char(std::span<const std::byte> bytes, std::size_t offset)
{
    return static_cast<char>(std::to_integer<unsigned char>(bytes[offset]));
}

std::string read_alpha(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::size_t width
)
{
    std::string text;

    for (std::size_t i = 0; i < width; ++i)
    {
        text.push_back(read_char(bytes, offset + i));
    }

    return text;
}

template <typename Bytes>
std::string to_hex(const Bytes& bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;

    for (auto b : bytes)
    {
        const auto v = static_cast<unsigned>(b);
        hex.push_back(digits[v >> 4]);
        hex.push_back(digits[v & 0xf]);
    }

    return hex;
}

// Asserts that `framed` is exactly one frame whose message equals the first
// `compare` bytes of `real` (the whole message unless `compare` is given).
void expect_matches_real(
    std::span<const std::byte> framed,
    std::span<const std::uint8_t> real,
    std::size_t compare = 0
)
{
    if (compare == 0)
    {
        compare = real.size();
    }

    ASSERT_EQ(framed.size(), real.size() + 2);
    EXPECT_EQ(read_be(framed, 0, 2), real.size());
    EXPECT_EQ(
        to_hex(framed.subspan(2, compare)),
        to_hex(real.first(compare))
    );
}

// First message of each type in data/itch/12302019.NASDAQ_ITCH50 (message
// bytes only, without the 2-byte length prefix).

// A: locate 496, tracking 0, ts 14400000724386, ref 8881, 'B', 2000, ARGX, 1605800
constexpr std::uint8_t real_a[] = {
    0x41, 0x01, 0xf0, 0x00, 0x00, 0x0d, 0x18, 0xc2, 0xed, 0x8d, 0xa2, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0xb1, 0x42, 0x00, 0x00, 0x07, 0xd0,
    0x41, 0x52, 0x47, 0x58, 0x20, 0x20, 0x20, 0x20, 0x00, 0x18, 0x80, 0xa8,
};

// F: locate 8899, tracking 0, ts 14400063421370, ref 15732, 'S', 100, ZVZZT, 15300000, LEHM
constexpr std::uint8_t real_f[] = {
    0x46, 0x22, 0xc3, 0x00, 0x00, 0x0d, 0x18, 0xc6, 0xaa, 0x3b, 0xba, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x3d, 0x74, 0x53, 0x00, 0x00, 0x00, 0x64,
    0x5a, 0x56, 0x5a, 0x5a, 0x54, 0x20, 0x20, 0x20, 0x00, 0xe9, 0x75, 0xa0,
    0x4c, 0x45, 0x48, 0x4d,
};

// E: locate 6966, tracking 2, ts 14400303314576, ref 39411, 3 shares, match 17791
constexpr std::uint8_t real_e[] = {
    0x45, 0x1b, 0x36, 0x00, 0x02, 0x0d, 0x18, 0xd4, 0xf6, 0xb6, 0x90, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x99, 0xf3, 0x00, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0x7f,
};

// C: locate 7603, tracking 1, ts 34086523596953, ref 9489356, 100 shares,
//    match 50460, printable 'N', price 41800
constexpr std::uint8_t real_c[] = {
    0x43, 0x1d, 0xb3, 0x00, 0x01, 0x1f, 0x00, 0x63, 0x21, 0x70, 0x99, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x90, 0xcb, 0xcc, 0x00, 0x00, 0x00, 0x64, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xc5, 0x1c, 0x4e, 0x00, 0x00, 0xa3, 0x48,
};

// X: locate 5301, tracking 0, ts 14400033107916, ref 8499, 500 shares
constexpr std::uint8_t real_x[] = {
    0x58, 0x14, 0xb5, 0x00, 0x00, 0x0d, 0x18, 0xc4, 0xdb, 0xaf, 0xcc, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x33, 0x00, 0x00, 0x01, 0xf4,
};

// D: locate 6644, tracking 0, ts 14400019074978, ref 983
constexpr std::uint8_t real_d[] = {
    0x44, 0x19, 0xf4, 0x00, 0x00, 0x0d, 0x18, 0xc4, 0x05, 0x8f, 0xa2, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xd7,
};

// U: locate 3816, tracking 0, ts 14400031359987, old 70, new 13502, 500 shares, 391400
constexpr std::uint8_t real_u[] = {
    0x55, 0x0e, 0xe8, 0x00, 0x00, 0x0d, 0x18, 0xc4, 0xc1, 0x03, 0xf3, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x34, 0xbe, 0x00, 0x00, 0x01, 0xf4, 0x00, 0x05, 0xf8, 0xe8,
};

// R: locate 1, tracking 0, ts 11354325413976, stock "A". Only the first 19
// bytes (header + stock) are compared; the builder fixes the rest.
constexpr std::uint8_t real_r[] = {
    0x52, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x53, 0xa2, 0x88, 0x70, 0x58, 0x41,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x4e, 0x20, 0x00, 0x00, 0x00,
    0x64, 0x4e, 0x43, 0x5a, 0x20, 0x50, 0x4e, 0x20, 0x31, 0x4e, 0x00, 0x00,
    0x00, 0x00, 0x4e,
};

} // namespace

TEST(ItchBuilderTest, EachMessageHasSpecLength)
{
    struct Case
    {
        ItchBuilder builder;
        std::size_t length;
    };

    std::vector<Case> cases;
    cases.push_back({ItchBuilder{}.add_order(1, 2, 3, 'B', 4, "AAPL", 5), 36});
    cases.push_back({ItchBuilder{}.add_order_mpid(1, 2, 3, 'S', 4, "AAPL", 5, "MPID"), 40});
    cases.push_back({ItchBuilder{}.executed(1, 2, 3, 4, 5), 31});
    cases.push_back({ItchBuilder{}.executed_with_price(1, 2, 3, 4, 5, true, 6), 36});
    cases.push_back({ItchBuilder{}.cancel(1, 2, 3, 4), 23});
    cases.push_back({ItchBuilder{}.erase(1, 2, 3), 19});
    cases.push_back({ItchBuilder{}.replace(1, 2, 3, 4, 5, 6), 35});
    cases.push_back({ItchBuilder{}.stock_directory(1, 2, "AAPL"), 39});

    for (const auto& c : cases)
    {
        const auto bytes = c.builder.bytes();
        SCOPED_TRACE(read_char(bytes, type_at));

        ASSERT_EQ(bytes.size(), c.length + 2);
        EXPECT_EQ(read_be(bytes, 0, 2), c.length);
    }
}

TEST(ItchBuilderTest, AddOrderPlacesEveryField)
{
    ItchBuilder builder;
    builder
        .tracking(0x0a0b)
        .add_order(0x0102, 0x112233445566, 0x0102030405060708, 'S', 0xa1b2c3d4, "AAPL", 0x01020304);

    const auto bytes = builder.bytes();

    EXPECT_EQ(read_char(bytes, type_at), 'A');
    EXPECT_EQ(read_be(bytes, locate_at, 2), 0x0102u);
    EXPECT_EQ(read_be(bytes, tracking_at, 2), 0x0a0bu);
    EXPECT_EQ(read_be(bytes, timestamp_at, 6), 0x112233445566u);
    EXPECT_EQ(read_be(bytes, body_at, 8), 0x0102030405060708u);
    EXPECT_EQ(read_char(bytes, body_at + 8), 'S');
    EXPECT_EQ(read_be(bytes, body_at + 9, 4), 0xa1b2c3d4u);
    EXPECT_EQ(read_alpha(bytes, body_at + 13, 8), "AAPL    ");
    EXPECT_EQ(read_be(bytes, body_at + 21, 4), 0x01020304u);
}

TEST(ItchBuilderTest, ReplacePlacesEveryField)
{
    ItchBuilder builder;
    builder
        .tracking(7)
        .replace(13, 0x0000aabbccddeeff, 0x1111111111111111, 0x2222222222222222, 300, 1500000);

    const auto bytes = builder.bytes();

    EXPECT_EQ(read_char(bytes, type_at), 'U');
    EXPECT_EQ(read_be(bytes, locate_at, 2), 13u);
    EXPECT_EQ(read_be(bytes, tracking_at, 2), 7u);
    EXPECT_EQ(read_be(bytes, timestamp_at, 6), 0xaabbccddeeffu);
    EXPECT_EQ(read_be(bytes, body_at, 8), 0x1111111111111111u);
    EXPECT_EQ(read_be(bytes, body_at + 8, 8), 0x2222222222222222u);
    EXPECT_EQ(read_be(bytes, body_at + 16, 4), 300u);
    EXPECT_EQ(read_be(bytes, body_at + 20, 4), 1500000u);
}

TEST(ItchBuilderTest, ExecutedWithPricePlacesEveryField)
{
    ItchBuilder builder;
    builder.executed_with_price(13, 42, 0x0102030405060708, 250, 0x1112131415161718, true, 1234500);
    builder.executed_with_price(13, 43, 9, 1, 2, false, 3);

    const auto bytes = builder.bytes();

    EXPECT_EQ(read_char(bytes, type_at), 'C');
    EXPECT_EQ(read_be(bytes, locate_at, 2), 13u);
    EXPECT_EQ(read_be(bytes, tracking_at, 2), 0u);
    EXPECT_EQ(read_be(bytes, timestamp_at, 6), 42u);
    EXPECT_EQ(read_be(bytes, body_at, 8), 0x0102030405060708u);
    EXPECT_EQ(read_be(bytes, body_at + 8, 4), 250u);
    EXPECT_EQ(read_be(bytes, body_at + 12, 8), 0x1112131415161718u);
    EXPECT_EQ(read_char(bytes, body_at + 20), 'Y');
    EXPECT_EQ(read_be(bytes, body_at + 21, 4), 1234500u);

    // Second frame starts at 38; its printable flag is 'N'.
    EXPECT_EQ(read_char(bytes, 38 + body_at + 20), 'N');
}

TEST(ItchBuilderTest, MessagesAreChainedBackToBack)
{
    ItchBuilder builder;
    builder
        .stock_directory(13, 1, "AAPL")
        .add_order(13, 2, 1, 'B', 100, "AAPL", 1000000)
        .add_order_mpid(13, 3, 2, 'S', 100, "AAPL", 1010000, "GSCO")
        .executed(13, 4, 1, 50, 1)
        .executed_with_price(13, 5, 1, 10, 2, true, 1000100)
        .cancel(13, 6, 1, 20)
        .replace(13, 7, 2, 3, 80, 1020000)
        .erase(13, 8, 3);

    const auto bytes = builder.bytes();
    const std::string expected_types = "RAFECXUD";

    std::size_t offset = 0;
    std::string types;

    while (offset < bytes.size())
    {
        ASSERT_LE(offset + 2, bytes.size());
        types.push_back(read_char(bytes, offset + type_at));
        EXPECT_EQ(read_be(bytes, offset + locate_at, 2), 13u);
        EXPECT_EQ(read_be(bytes, offset + timestamp_at, 6), types.size());
        offset += 2 + read_be(bytes, offset, 2);
    }

    EXPECT_EQ(offset, bytes.size());
    EXPECT_EQ(types, expected_types);
    EXPECT_EQ(bytes.size(), (39 + 36 + 40 + 31 + 36 + 23 + 35 + 19) + 8 * 2u);
}

TEST(ItchBuilderTest, AddOrderMatchesRealNasdaqBytes)
{
    ItchBuilder builder;
    builder.tracking(0).add_order(496, 14400000724386, 8881, 'B', 2000, "ARGX", 1605800);

    expect_matches_real(builder.bytes(), real_a);
}

TEST(ItchBuilderTest, AddOrderMpidMatchesRealNasdaqBytes)
{
    ItchBuilder builder;
    builder.tracking(0).add_order_mpid(8899, 14400063421370, 15732, 'S', 100, "ZVZZT", 15300000, "LEHM");

    expect_matches_real(builder.bytes(), real_f);
}

TEST(ItchBuilderTest, ExecutedMatchesRealNasdaqBytes)
{
    ItchBuilder builder;
    builder.tracking(2).executed(6966, 14400303314576, 39411, 3, 17791);

    expect_matches_real(builder.bytes(), real_e);
}

TEST(ItchBuilderTest, ExecutedWithPriceMatchesRealNasdaqBytes)
{
    ItchBuilder builder;
    builder.tracking(1).executed_with_price(7603, 34086523596953, 9489356, 100, 50460, false, 41800);

    expect_matches_real(builder.bytes(), real_c);
}

TEST(ItchBuilderTest, CancelMatchesRealNasdaqBytes)
{
    ItchBuilder builder;
    builder.tracking(0).cancel(5301, 14400033107916, 8499, 500);

    expect_matches_real(builder.bytes(), real_x);
}

TEST(ItchBuilderTest, EraseMatchesRealNasdaqBytes)
{
    ItchBuilder builder;
    builder.tracking(0).erase(6644, 14400019074978, 983);

    expect_matches_real(builder.bytes(), real_d);
}

TEST(ItchBuilderTest, ReplaceMatchesRealNasdaqBytes)
{
    ItchBuilder builder;
    builder.tracking(0).replace(3816, 14400031359987, 70, 13502, 500, 391400);

    expect_matches_real(builder.bytes(), real_u);
}

TEST(ItchBuilderTest, StockDirectoryHeaderAndStockMatchRealNasdaqBytes)
{
    ItchBuilder builder;
    builder.tracking(0).stock_directory(1, 11354325413976, "A");

    expect_matches_real(builder.bytes(), real_r, 19);
}

} // namespace simulator::itch_test
