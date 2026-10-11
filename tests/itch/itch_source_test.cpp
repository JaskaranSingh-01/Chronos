#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <tuple>
#include <vector>

#include "itch_test_builder.hpp"
#include "simulator/itch/itch_source.hpp"

namespace simulator {
namespace {

using namespace std::chrono_literals;
using itch_test::ItchBuilder;

// 2019-12-30 00:00 at -05:00, as epoch ns.
constexpr Timestamp midnight = 1'577'682'000'000'000'000;

constexpr ItchSourceConfig aapl{
    .ticker = "AAPL", .session_date = 2019y / 12 / 30, .utc_offset = -5h};

constexpr Instrument xnas_aapl{
    .venue = {"XNAS"}, .native_symbol = {"AAPL"}, .price_scale = 4, .quantity_scale = 0,
    .tick_size = 0};

auto fields(const MarketEvent& e)
{
    return std::tuple{e.timestamp, e.order_id, e.new_order_id, e.price.ticks,
                      e.quantity.value(), e.symbol, static_cast<int>(e.type),
                      static_cast<int>(e.side)};
}

// Reads events until the stream ends; the end result goes to `end`.
std::vector<MarketEvent> drain(ItchSource& source, SourceResult& end)
{
    std::vector<MarketEvent> events;
    MarketEvent event;

    while ((end = source.next(event)) == SourceResult::Event) {
        events.push_back(event);
    }
    return events;
}

TEST(ItchSourceTest, RegistersItsInstrumentFromR)
{
    ItchBuilder itch;
    itch.stock_directory(7, 1, "AAPL").add_order(7, 2, 1, 'B', 100, "AAPL", 2'900'000);

    InstrumentRegistry registry{4};
    SymbolId other{};
    registry.add(Instrument{.venue = {"XNAS"}, .native_symbol = {"MSFT"}}, other);  // id 0

    ItchSource source{itch.bytes(), aapl, registry};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(source.locate(), 7);
    EXPECT_EQ(event.symbol, 1u);
    EXPECT_EQ(source.symbol(), 1u);
    ASSERT_NE(registry.find(1), nullptr);
    EXPECT_EQ(*registry.find(1), xnas_aapl);
}

TEST(ItchSourceTest, FiltersOtherLocatesWithoutDecodingThem)
{
    ItchBuilder itch;
    itch.system_event(0, 1, 'O')
        .stock_directory(1, 2, "AAPL")
        .stock_directory(2, 3, "MSFT")
        .add_order(2, 4, 1, 'Z', 100, "MSFT", 1'500'000)  // bad side, but not ours
        .add_order(1, 5, 2, 'S', 100, "AAPL", 2'900'000)
        .erase(3, 6, 3);

    InstrumentRegistry registry{4};
    ItchSource source{itch.bytes(), aapl, registry};
    SourceResult end{};
    const auto events = drain(source, end);

    EXPECT_EQ(end, SourceResult::EndOfStream);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].order_id, 2u);
    EXPECT_EQ(registry.size(), 1u);

    const ItchSourceStatistics& stats = source.statistics();
    EXPECT_EQ(stats.messages, 6u);
    EXPECT_EQ(stats.skipped, 1u);   // S
    EXPECT_EQ(stats.filtered, 3u);  // R MSFT, A locate 2, D locate 3
    EXPECT_EQ(stats.events, 1u);
}

TEST(ItchSourceTest, MapsEachMessageType)
{
    ItchBuilder itch;
    itch.stock_directory(1, 1, "AAPL")
        .add_order(1, 10, 1, 'B', 300, "AAPL", 2'900'000)
        .add_order_mpid(1, 11, 2, 'S', 200, "AAPL", 2'910'000, "GSCO")
        .executed(1, 12, 1, 100, 900)
        .executed_with_price(1, 13, 1, 50, 901, true, 2'899'900)
        .cancel(1, 14, 2, 20)
        .replace(1, 15, 2, 3, 150, 2'905'000)
        .erase(1, 16, 3)
        .trade(1, 17, 0, 'B', 40, "AAPL", 2'900'500, 902);

    InstrumentRegistry registry{4};
    ItchSource source{itch.bytes(), aapl, registry};
    SourceResult end{};
    const auto events = drain(source, end);

    using T = MarketEventType;
    const MarketEvent expected[] = {
        {.timestamp = midnight + 10, .order_id = 1, .price = Price{2'900'000},
         .quantity = Quantity{300}, .type = T::Add, .side = Side::Buy},
        {.timestamp = midnight + 11, .order_id = 2, .price = Price{2'910'000},
         .quantity = Quantity{200}, .type = T::Add, .side = Side::Sell},
        {.timestamp = midnight + 12, .order_id = 1, .quantity = Quantity{100}, .type = T::Execute},
        {.timestamp = midnight + 13, .order_id = 1, .price = Price{2'899'900},
         .quantity = Quantity{50}, .type = T::Execute},
        {.timestamp = midnight + 14, .order_id = 2, .quantity = Quantity{20}, .type = T::Cancel},
        {.timestamp = midnight + 15, .order_id = 2, .new_order_id = 3, .price = Price{2'905'000},
         .quantity = Quantity{150}, .type = T::Replace},
        {.timestamp = midnight + 16, .order_id = 3, .type = T::Delete},
        {.timestamp = midnight + 17, .order_id = 0, .price = Price{2'900'500},
         .quantity = Quantity{40}, .type = T::Trade, .side = Side::Buy},
    };

    EXPECT_EQ(end, SourceResult::EndOfStream);
    ASSERT_EQ(events.size(), std::size(expected));
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(fields(events[i]), fields(expected[i])) << "event " << i;
    }
}

TEST(ItchSourceTest, TimestampsAreEpochNsFromSessionMidnight)
{
    constexpr std::uint64_t open = 34'200'000'000'000;  // 09:30 local
    ItchBuilder itch;
    itch.stock_directory(1, 1, "AAPL").add_order(1, open, 1, 'B', 100, "AAPL", 2'900'000);

    InstrumentRegistry registry{4};
    ItchSource source{itch.bytes(), aapl, registry};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(event.timestamp, 1'577'682'000'000'000'000 + 34'200'000'000'000);  // 14:30 UTC
}

TEST(ItchSourceTest, SkipsAMessageForItsLocateBeforeItsR)
{
    ItchBuilder itch;
    itch.add_order(1, 1, 1, 'B', 100, "AAPL", 2'900'000)
        .stock_directory(1, 2, "AAPL")
        .add_order(1, 3, 2, 'B', 100, "AAPL", 2'900'000);

    InstrumentRegistry registry{4};
    ItchSource source{itch.bytes(), aapl, registry};
    SourceResult end{};
    const auto events = drain(source, end);

    EXPECT_EQ(end, SourceResult::EndOfStream);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].order_id, 2u);
}

TEST(ItchSourceTest, NoRForTheTickerIsSymbolNotFoundAtTheEnd)
{
    ItchBuilder itch;
    itch.stock_directory(2, 1, "MSFT").add_order(2, 2, 1, 'B', 100, "MSFT", 1'500'000);

    InstrumentRegistry registry{4};
    ItchSource source{itch.bytes(), aapl, registry};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Error);
    EXPECT_EQ(source.error(), SourceError::SymbolNotFound);
    EXPECT_EQ(source.position(), itch.bytes().size());
}

TEST(ItchSourceTest, AFullRegistryIsInstrumentRejected)
{
    ItchBuilder itch;
    itch.system_event(0, 1, 'O').stock_directory(1, 2, "AAPL");

    InstrumentRegistry registry{0};
    ItchSource source{itch.bytes(), aapl, registry};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Error);
    EXPECT_EQ(source.error(), SourceError::InstrumentRejected);
    EXPECT_EQ(source.position(), 14u);  // after the 14-byte framed S
}

// R (41 framed bytes), A (38), A (38) cut short by 5 bytes.
class ItchSourceTruncationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        itch_.stock_directory(1, 1, "AAPL")
            .add_order(1, 2, 1, 'B', 100, "AAPL", 2'900'000)
            .add_order(1, 3, 2, 'B', 100, "AAPL", 2'900'000);
        cut_ = itch_.bytes().first(itch_.bytes().size() - 5);
    }

    ItchBuilder itch_;
    std::span<const std::byte> cut_;
    InstrumentRegistry registry_{4};
    static constexpr std::uint64_t last_message = 41 + 38;
};

TEST_F(ItchSourceTruncationTest, ATruncatedTailIsAnErrorByDefault)
{
    ItchSource source{cut_, aapl, registry_};
    SourceResult end{};
    const auto events = drain(source, end);

    EXPECT_EQ(events.size(), 1u);
    ASSERT_EQ(end, SourceResult::Error);
    EXPECT_EQ(source.error(), SourceError::TruncatedMessage);
    EXPECT_EQ(source.position(), last_message);
    EXPECT_FALSE(source.truncated_tail());
}

TEST_F(ItchSourceTruncationTest, ATruncatedTailEndsTheStreamWhenAllowed)
{
    ItchSourceConfig config = aapl;
    config.allow_truncated_tail = true;

    ItchSource source{cut_, config, registry_};
    SourceResult end{};
    const auto events = drain(source, end);
    MarketEvent event;

    EXPECT_EQ(events.size(), 1u);
    ASSERT_EQ(end, SourceResult::EndOfStream);
    EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
    EXPECT_TRUE(source.truncated_tail());
    EXPECT_EQ(source.position(), last_message);
}

TEST_F(ItchSourceTruncationTest, ACutLengthPrefixIsATruncatedTailToo)
{
    ItchSourceConfig config = aapl;
    config.allow_truncated_tail = true;

    ItchSource source{itch_.bytes().first(last_message + 1), config, registry_};
    SourceResult end{};
    drain(source, end);

    EXPECT_EQ(end, SourceResult::EndOfStream);
    EXPECT_TRUE(source.truncated_tail());
}

TEST(ItchSourceTest, AZeroLengthIsAnErrorEvenWhenTailsAreAllowed)
{
    ItchBuilder itch;
    itch.stock_directory(1, 1, "AAPL");
    std::vector<std::byte> bytes{itch.bytes().begin(), itch.bytes().end()};
    bytes.resize(bytes.size() + 3);  // u16 length 0, then a stray byte

    ItchSourceConfig config = aapl;
    config.allow_truncated_tail = true;

    InstrumentRegistry registry{4};
    ItchSource source{bytes, config, registry};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Error);
    EXPECT_EQ(source.error(), SourceError::InvalidLength);
    EXPECT_EQ(source.position(), 41u);
}

TEST(ItchSourceTest, ErrorsAreSticky)
{
    ItchBuilder itch;
    itch.stock_directory(1, 1, "AAPL")
        .add_order(1, 2, 1, 'Z', 100, "AAPL", 2'900'000)
        .add_order(1, 3, 2, 'B', 100, "AAPL", 2'900'000);

    InstrumentRegistry registry{4};
    ItchSource source{itch.bytes(), aapl, registry};
    MarketEvent event;

    for (int call = 0; call < 2; ++call) {
        ASSERT_EQ(source.next(event), SourceResult::Error) << "call " << call;
        EXPECT_EQ(source.error(), SourceError::InvalidSide);
        EXPECT_EQ(source.position(), 41u);
    }
}

} // namespace
} // namespace simulator
