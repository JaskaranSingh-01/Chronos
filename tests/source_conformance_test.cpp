#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "itch/itch_test_builder.hpp"
#include "simulator/csv/csv_source.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/vector_source.hpp"
#include "support/source_conformance.hpp"

namespace simulator::conformance {
namespace {

TEST_P(SourceConformanceTest, TimestampsNeverDecrease)
{
    EXPECT_EQ(check_time(*make(data_)), std::nullopt);
}

TEST_P(SourceConformanceTest, ReferencesNameLiveOrdersWithinTheirSize)
{
    EXPECT_EQ(check_references(*make(data_)), std::nullopt);
}

TEST_P(SourceConformanceTest, SymbolsAreRegisteredAndPricesAndSizesAreValid)
{
    EXPECT_EQ(check_scales(*make(data_), registry_), std::nullopt);
}

TEST_P(SourceConformanceTest, ReadingTwiceGivesIdenticalEvents)
{
    const auto first = make(data_);
    const auto second = make(data_);

    EXPECT_EQ(check_determinism(*first, *second), std::nullopt);
}

TEST_P(SourceConformanceTest, BadInputEndsInStickyError)
{
    const SourceCase& source_case = GetParam();

    if (source_case.expected_error == SourceError::None) {
        GTEST_SKIP() << source_case.name << " cannot fail";
    }

    EXPECT_EQ(
        check_error(
            *make(source_case.bad_input),
            source_case.expected_error,
            source_case.expected_position),
        std::nullopt);
}

constexpr Instrument xnas_aapl{
    .venue = {"XNAS"}, .native_symbol = {"AAPL"}, .price_scale = 4, .tick_size = 100};
constexpr Instrument xnas_msft{
    .venue = {"XNAS"}, .native_symbol = {"MSFT"}, .price_scale = 4, .tick_size = 0};

// Two symbols share order id 1, and every event type appears. VectorSource cannot fail.
constexpr MarketEvent vector_events[] = {
    {.timestamp = 1'000, .order_id = 1, .price = Price{1'000'100}, .quantity = Quantity{500},
     .symbol = 0, .type = MarketEventType::Add, .side = Side::Sell},
    {.timestamp = 1'000, .order_id = 1, .price = Price{4'000'001}, .quantity = Quantity{50},
     .symbol = 1, .type = MarketEventType::Add, .side = Side::Buy},
    {.timestamp = 1'500, .order_id = 1, .price = Price{1'000'100}, .quantity = Quantity{100},
     .symbol = 0, .type = MarketEventType::Execute},
    {.timestamp = 2'000, .order_id = 1, .quantity = Quantity{100},
     .symbol = 0, .type = MarketEventType::Cancel},
    {.timestamp = 2'000, .order_id = 1, .new_order_id = 2, .price = Price{1'000'200},
     .quantity = Quantity{300}, .symbol = 0, .type = MarketEventType::Replace},
    {.timestamp = 2'500, .order_id = 2, .price = Price{1'000'300}, .quantity = Quantity{200},
     .symbol = 0, .type = MarketEventType::Modify},
    {.timestamp = 3'000, .order_id = 2, .symbol = 0, .type = MarketEventType::Delete},
    {.timestamp = 3'000, .price = Price{4'000'005}, .quantity = Quantity{10},
     .symbol = 1, .type = MarketEventType::Trade, .side = Side::Sell},
    {.timestamp = 3'500, .order_id = 1, .quantity = Quantity{50},
     .symbol = 1, .type = MarketEventType::Execute},
    {.timestamp = 4'000, .order_id = 1, .price = Price{1'000'100}, .quantity = Quantity{10},
     .symbol = 0, .type = MarketEventType::Add, .side = Side::Buy},
    {.timestamp = 4'500, .symbol = 0, .type = MarketEventType::Clear},
    {.timestamp = 4'500, .symbol = 1, .type = MarketEventType::Status, .trading_state = TradingState::Halted},
};

std::unique_ptr<MarketEventSource> make_vector_source(
    std::span<const std::byte>, InstrumentRegistry& registry)
{
    SymbolId id{};
    registry.add(xnas_aapl, id);  // 0
    registry.add(xnas_msft, id);  // 1
    return std::make_unique<VectorSource>(vector_events);
}

INSTANTIATE_TEST_SUITE_P(
    Vector, SourceConformanceTest,
    ::testing::Values(SourceCase{.name = "Vector", .factory = make_vector_source}));

std::string to_string(std::span<const std::byte> bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Every mapped ITCH type for AAPL (H as Status), plus S messages (skipped; the
// start of system hours halts AAPL until its H) and MSFT messages (filtered).
const std::string itch_good = to_string(
    itch_test::ItchBuilder{}
        .system_event(0, 1, 'O')
        .stock_directory(1, 2, "AAPL")
        .stock_directory(2, 3, "MSFT")
        .system_event(0, 3, 'S')
        .trading_action(1, 4, "AAPL", 'T')
        .trading_action(2, 4, "MSFT", 'H')
        .add_order(2, 10, 1, 'B', 100, "MSFT", 1'500'000)
        .add_order(1, 10, 1, 'S', 500, "AAPL", 2'900'100)
        .add_order_mpid(1, 11, 2, 'B', 300, "AAPL", 2'899'900, "GSCO")
        .executed(1, 12, 1, 100, 1)
        .executed_with_price(1, 13, 1, 100, 2, true, 2'900'000)
        .cancel(1, 14, 2, 100)
        .replace(1, 15, 2, 3, 200, 2'899'800)
        .trade(1, 16, 0, 'B', 50, "AAPL", 2'900'050, 3)
        .erase(1, 17, 3)
        .executed(2, 18, 1, 100, 4)
        .executed(1, 19, 1, 300, 5)
        .bytes());

// R, then an A for AAPL with side 'Z': InvalidSide at the A, after the 41-byte R.
const std::string itch_bad = to_string(
    itch_test::ItchBuilder{}
        .stock_directory(1, 1, "AAPL")
        .add_order(1, 2, 1, 'Z', 100, "AAPL", 2'900'000)
        .bytes());

std::unique_ptr<MarketEventSource> make_itch_source(
    std::span<const std::byte> data, InstrumentRegistry& registry)
{
    using namespace std::chrono_literals;
    return std::make_unique<ItchSource>(
        data,
        ItchSourceConfig{.ticker = "AAPL", .session_date = 2019y / 12 / 30, .utc_offset = -5h},
        registry);
}

INSTANTIATE_TEST_SUITE_P(
    Itch, SourceConformanceTest,
    ::testing::Values(SourceCase{
        .name = "Itch", .data = itch_good, .factory = make_itch_source, .bad_input = itch_bad,
        .expected_error = SourceError::InvalidSide, .expected_position = 41}));

// basic.csv holds every CSV action at xnas_aapl's scale 4 and tick 100; the bad
// input has a negative quantity.
std::unique_ptr<MarketEventSource> make_csv_source(
    std::span<const std::byte> data, InstrumentRegistry& registry)
{
    SymbolId id{};
    registry.add(xnas_aapl, id);
    return std::make_unique<CsvSource>(data, id);
}

INSTANTIATE_TEST_SUITE_P(
    Csv, SourceConformanceTest,
    ::testing::Values(SourceCase{
        .name = "Csv", .data_file = "basic.csv", .factory = make_csv_source,
        .bad_input = "timestamp,event_type,order_id,side,price,quantity,new_order_id\n"
                     "1577716200000000000,ADD,1,BUY,1000000,-5,\n",
        .expected_error = SourceError::NumberOutOfRange, .expected_position = 2}));

// Emits `events`, then a sticky EndOfStream, or a sticky Error at position
// events.size() + 1 when `error` is not None.
class ScriptedSource final : public MarketEventSource
{
private:
    std::span<const MarketEvent> events_;
    SourceError error_;
    std::uint64_t position_{0};

public:
    explicit ScriptedSource(
        std::span<const MarketEvent> events,
        SourceError error = SourceError::None) noexcept
        : events_(events), error_(error) {}

    SourceResult next(MarketEvent& event) noexcept override
    {
        if (position_ < events_.size()) {
            event = events_[position_++];
            return SourceResult::Event;
        }
        if (error_ == SourceError::None) {
            return SourceResult::EndOfStream;
        }
        position_ = events_.size() + 1;
        return SourceResult::Error;
    }

    std::uint64_t position() const noexcept override
    {
        return position_;
    }

    SourceError error() const noexcept override
    {
        return error_;
    }
};

constexpr MarketEvent add(Timestamp timestamp, OrderId id)
{
    return MarketEvent{
        .timestamp = timestamp, .order_id = id, .price = Price{100}, .quantity = Quantity{10}};
}

constexpr MarketEvent cancel(Timestamp timestamp, OrderId id)
{
    return MarketEvent{
        .timestamp = timestamp, .order_id = id, .quantity = Quantity{10},
        .type = MarketEventType::Cancel};
}

bool mentions(const std::optional<std::string>& violation, const std::string& text)
{
    return violation && violation->find(text) != std::string::npos;
}

TEST(SourceConformanceCheckTest, TimeCheckReportsATimestampGoingBackwards)
{
    const MarketEvent ties[] = {add(100, 1), add(100, 2), add(200, 3)};
    const MarketEvent backwards[] = {add(100, 1), add(200, 2), add(150, 3)};

    ScriptedSource good{ties};
    ScriptedSource broken{backwards};

    EXPECT_EQ(check_time(good), std::nullopt);

    const auto violation = check_time(broken);
    EXPECT_TRUE(mentions(violation, "position 3")) << violation.value_or("no violation");
}

TEST(SourceConformanceCheckTest, ReferenceCheckReportsACancelOfAnUnknownId)
{
    const MarketEvent known[] = {add(100, 1), cancel(200, 1)};
    const MarketEvent unknown[] = {add(100, 1), cancel(200, 9)};

    ScriptedSource good{known};
    ScriptedSource broken{unknown};

    EXPECT_EQ(check_references(good), std::nullopt);

    const auto violation = check_references(broken);
    EXPECT_TRUE(mentions(violation, "position 2")) << violation.value_or("no violation");
}

TEST(SourceConformanceCheckTest, ScaleCheckKeepsTheTradingStateToStatusEvents)
{
    constexpr MarketEvent halt{.symbol = 0, .type = MarketEventType::Status,
                               .trading_state = TradingState::QuotationOnly};
    MarketEvent halt_with_size = halt;
    halt_with_size.quantity = Quantity{10};
    MarketEvent unknown_state = halt;
    unknown_state.trading_state = static_cast<TradingState>(4);
    MarketEvent add_with_state = add(100, 1);
    add_with_state.trading_state = TradingState::Halted;

    InstrumentRegistry registry{1};
    SymbolId id{};
    registry.add(xnas_aapl, id);

    const MarketEvent good[] = {add(100, 1), halt};
    ScriptedSource good_source{good};
    EXPECT_EQ(check_scales(good_source, registry), std::nullopt);

    for (const MarketEvent& broken : {halt_with_size, unknown_state, add_with_state}) {
        const MarketEvent events[] = {add(100, 1), broken};
        ScriptedSource source{events};
        const auto violation = check_scales(source, registry);
        EXPECT_TRUE(mentions(violation, "position 2")) << violation.value_or("no violation");
    }
}

TEST(SourceConformanceCheckTest, ErrorCheckWantsTheExpectedStickyError)
{
    const MarketEvent events[] = {add(100, 1)};

    ScriptedSource failing{events, SourceError::InvalidRow};
    ScriptedSource wrong_error{events, SourceError::InvalidRow};
    ScriptedSource wrong_position{events, SourceError::InvalidRow};
    ScriptedSource ending{events};

    EXPECT_EQ(check_error(failing, SourceError::InvalidRow, 2), std::nullopt);
    EXPECT_NE(check_error(wrong_error, SourceError::InvalidNumber, 2), std::nullopt);
    EXPECT_NE(check_error(wrong_position, SourceError::InvalidRow, 1), std::nullopt);
    EXPECT_NE(check_error(ending, SourceError::InvalidRow, 2), std::nullopt);
}

} // namespace
} // namespace simulator::conformance
