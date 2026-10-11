#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "simulator/market/vector_source.hpp"
#include "support/source_conformance.hpp"

namespace simulator::conformance {
namespace {

// ---------------------------------------------------------------------------
// The suite every MarketEventSource must pass. Each source adds one
// INSTANTIATE_TEST_SUITE_P line at the bottom of this file.
// ---------------------------------------------------------------------------

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

// VectorSource: events from an in-test array, so no data file. Two symbols
// share order id 1, and every event type appears. It cannot fail.
constexpr Instrument xnas_aapl{
    .venue = {"XNAS"}, .native_symbol = {"AAPL"}, .price_scale = 4, .tick_size = 100};
constexpr Instrument xnas_msft{
    .venue = {"XNAS"}, .native_symbol = {"MSFT"}, .price_scale = 4, .tick_size = 0};

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

// ---------------------------------------------------------------------------
// The checks themselves, run against deliberately broken in-test sources.
// ---------------------------------------------------------------------------

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
