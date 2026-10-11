#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "simulator/market/market_replay.hpp"
#include "simulator/market/vector_source.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 3;

MarketEvent event(Timestamp ts, MarketEventType type, OrderId id, std::uint64_t quantity = 0,
                  std::int64_t price = 0, Side side = Side::Buy)
{
    return MarketEvent{.timestamp = ts, .order_id = id, .price = Price{price},
                       .quantity = Quantity{quantity}, .symbol = kSymbol, .type = type,
                       .side = side};
}

MarketEvent add(Timestamp ts, OrderId id, Side side, std::int64_t price, std::uint64_t quantity)
{
    return event(ts, MarketEventType::Add, id, quantity, price, side);
}

std::uint64_t by_type(const ReplayReport& report, MarketEventType type)
{
    return report.by_type[static_cast<std::size_t>(type)];
}

std::uint64_t by_result(const ReplayReport& report, ApplyResult result)
{
    return report.by_result[static_cast<std::size_t>(result)];
}

// VectorSource never fails: this one fails with `error` at `position` once
// its events run out.
class FailingSource final : public MarketEventSource
{
    VectorSource events_;
    SourceError error_;
    std::uint64_t position_;
    bool failed_{false};

public:
    FailingSource(std::span<const MarketEvent> events, SourceError error, std::uint64_t position)
        : events_(events), error_(error), position_(position) {}

    SourceResult next(MarketEvent& e) noexcept override
    {
        if (!failed_ && events_.next(e) == SourceResult::Event) {
            return SourceResult::Event;
        }
        failed_ = true;
        return SourceResult::Error;
    }
    std::uint64_t position() const noexcept override { return failed_ ? position_ : events_.position(); }
    SourceError error() const noexcept override { return failed_ ? error_ : SourceError::None; }
};

TEST(MarketReplayTest, CountsEveryEventByTypeAndResult)
{
    using T = MarketEventType;
    std::vector<MarketEvent> input{
        add(10, 1, Side::Buy, 100, 500),
        add(10, 2, Side::Sell, 101, 300),  // equal timestamps are in order
        event(20, T::Execute, 1, 100),
        event(30, T::Cancel, 2, 100),
        event(40, T::Replace, 2, 200),     // new_order_id set below
        event(50, T::Trade, 0, 50, 100),
        event(60, T::Delete, 1)};
    input[4].new_order_id = 3;
    input[4].price = Price{102};

    VectorSource source{input};
    MarketBook book{kSymbol, 8};
    const ReplayReport report = MarketReplay::run(source, book, {});

    EXPECT_TRUE(report.succeeded());
    EXPECT_EQ(report.failure, ReplayFailure::None);
    EXPECT_EQ(report.message, "");
    EXPECT_EQ(report.events, 7u);
    EXPECT_EQ(report.applied(), 7u);
    EXPECT_EQ(report.rejected(), 0u);
    EXPECT_EQ(by_type(report, T::Add), 2u);
    EXPECT_EQ(by_type(report, T::Execute), 1u);
    EXPECT_EQ(by_type(report, T::Cancel), 1u);
    EXPECT_EQ(by_type(report, T::Replace), 1u);
    EXPECT_EQ(by_type(report, T::Trade), 1u);
    EXPECT_EQ(by_type(report, T::Delete), 1u);
    EXPECT_EQ(by_type(report, T::Modify), 0u);
    EXPECT_EQ(by_result(report, ApplyResult::Applied), 7u);
    EXPECT_EQ(report.first_timestamp, 10);
    EXPECT_EQ(report.last_timestamp, 60);
    EXPECT_EQ(report.invariant_checks, 0u);

    // The book saw every event: only the replacement (id 3) is left.
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.quantity_at(Side::Sell, Price{102}), 200u);
}

TEST(MarketReplayTest, EmptySourceSucceedsWithNothingCounted)
{
    VectorSource source{std::span<const MarketEvent>{}};
    MarketBook book{kSymbol, 1};
    const ReplayReport report = MarketReplay::run(source, book, {.check_invariants_every = 1});

    EXPECT_TRUE(report.succeeded());
    EXPECT_EQ(report, ReplayReport{});
}

TEST(MarketReplayTest, CountsRejectionsByReasonAndKeepsGoing)
{
    using T = MarketEventType;
    MarketEvent wrong_symbol = add(7, 9, Side::Buy, 100, 10);
    wrong_symbol.symbol = kSymbol + 1;

    const std::vector<MarketEvent> input{
        add(1, 1, Side::Buy, 100, 10),
        add(2, 1, Side::Buy, 100, 10),   // DuplicateOrder
        event(3, T::Delete, 42),         // UnknownOrder
        event(4, T::Execute, 1, 11),     // ExceedsRemaining
        add(5, 2, Side::Buy, 100, 0),    // ZeroQuantity
        event(6, T::Modify, 1, 5, 100),  // Unsupported
        wrong_symbol,                    // WrongSymbol
        event(8, T::Execute, 1, 10)};    // still applied after all of that

    VectorSource source{input};
    MarketBook book{kSymbol, 4};
    const ReplayReport report = MarketReplay::run(source, book, {});

    EXPECT_TRUE(report.succeeded());
    EXPECT_EQ(report.events, 8u);
    EXPECT_EQ(report.applied(), 2u);
    EXPECT_EQ(report.rejected(), 6u);
    EXPECT_EQ(by_result(report, ApplyResult::DuplicateOrder), 1u);
    EXPECT_EQ(by_result(report, ApplyResult::UnknownOrder), 1u);
    EXPECT_EQ(by_result(report, ApplyResult::ExceedsRemaining), 1u);
    EXPECT_EQ(by_result(report, ApplyResult::ZeroQuantity), 1u);
    EXPECT_EQ(by_result(report, ApplyResult::Unsupported), 1u);
    EXPECT_EQ(by_result(report, ApplyResult::WrongSymbol), 1u);
    EXPECT_EQ(by_type(report, T::Add), 4u);  // rejected events still count by type
    EXPECT_EQ(by_type(report, T::Execute), 2u);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(MarketReplayTest, OutOfOrderTimestampIsFatalAndNotApplied)
{
    const std::vector<MarketEvent> input{
        add(10, 1, Side::Buy, 100, 10),
        add(20, 2, Side::Buy, 100, 10),
        add(19, 3, Side::Buy, 100, 10),  // earlier than the event before it
        add(30, 4, Side::Buy, 100, 10)};

    VectorSource source{input};
    MarketBook book{kSymbol, 8};
    const ReplayReport report = MarketReplay::run(source, book, {});

    EXPECT_FALSE(report.succeeded());
    EXPECT_EQ(report.failure, ReplayFailure::OutOfOrderTimestamp);
    EXPECT_EQ(report.failed_position, 3u);  // VectorSource: 1-based index
    EXPECT_FALSE(report.message.empty());
    EXPECT_EQ(report.source_error, SourceError::None);
    EXPECT_EQ(report.events, 2u);
    EXPECT_EQ(report.last_timestamp, 20);
    EXPECT_EQ(book.order_count(), 2u);  // neither 3 nor 4 reached the book
}

TEST(MarketReplayTest, SourceErrorIsFatalAndPropagated)
{
    const std::vector<MarketEvent> input{add(10, 1, Side::Buy, 100, 10)};

    FailingSource source{input, SourceError::TruncatedMessage, 1234};
    MarketBook book{kSymbol, 4};
    const ReplayReport report = MarketReplay::run(source, book, {});

    EXPECT_FALSE(report.succeeded());
    EXPECT_EQ(report.failure, ReplayFailure::SourceError);
    EXPECT_EQ(report.source_error, SourceError::TruncatedMessage);
    EXPECT_EQ(report.failed_position, 1234u);
    EXPECT_EQ(report.message, "TruncatedMessage");
    EXPECT_EQ(report.events, 1u);  // what came before the error was applied
    EXPECT_EQ(report.applied(), 1u);
}

TEST(MarketReplayTest, ChecksInvariantsEveryNAppliedEvents)
{
    std::vector<MarketEvent> input;
    for (OrderId id = 1; id <= 7; ++id) {
        input.push_back(add(static_cast<Timestamp>(id), id, Side::Buy, 100, 10));
    }
    input.push_back(add(8, 1, Side::Buy, 100, 10));  // rejected: does not count towards N

    VectorSource source{input};
    MarketBook book{kSymbol, 16};
    const ReplayReport report = MarketReplay::run(source, book, {.check_invariants_every = 3});

    EXPECT_TRUE(report.succeeded());
    EXPECT_EQ(report.applied(), 7u);
    EXPECT_EQ(report.invariant_checks, 2u);  // after applied events 3 and 6
}

TEST(MarketReplayTest, CrossedBookFailsTheInvariantCheckOnlyInRth)
{
    constexpr Timestamp midnight = 1'577'682'000'000'000'000;  // 2019-12-30, -05:00
    constexpr Timestamp pre_market = midnight + RTH_OPEN_NS - 1;
    constexpr Timestamp open = midnight + RTH_OPEN_NS;

    // A bid above the ask: legitimate pre-market, a violation in RTH.
    const std::vector<MarketEvent> before_open{
        add(pre_market, 1, Side::Buy, 101, 10), add(pre_market, 2, Side::Sell, 100, 10)};
    const std::vector<MarketEvent> after_open{
        add(open, 1, Side::Buy, 101, 10), add(open, 2, Side::Sell, 100, 10),
        add(open, 3, Side::Sell, 102, 10)};
    const ReplayOptions options{.check_invariants_every = 1, .session_midnight = midnight};

    VectorSource quiet{before_open};
    MarketBook quiet_book{kSymbol, 4};
    EXPECT_TRUE(MarketReplay::run(quiet, quiet_book, options).succeeded());

    VectorSource crossed{after_open};
    MarketBook book{kSymbol, 4};
    const ReplayReport report = MarketReplay::run(crossed, book, options);

    EXPECT_EQ(report.failure, ReplayFailure::InvariantViolation);
    EXPECT_EQ(report.invariant_error, InvariantError::CrossedInRth);
    EXPECT_EQ(report.message, "CrossedInRth");
    EXPECT_EQ(report.failed_position, 2u);
    EXPECT_EQ(report.invariant_checks, 2u);
    EXPECT_EQ(report.events, 2u);  // the first violation stops the run
}

TEST(MarketReplayTest, SameInputGivesSameReportAndDigest)
{
    using T = MarketEventType;
    std::mt19937_64 rng{42};
    std::vector<MarketEvent> input;
    OrderId next_id = 1;

    for (Timestamp ts = 0; ts < 5'000; ++ts) {
        const OrderId id = 1 + rng() % next_id;  // sometimes unknown: rejections are fine
        switch (rng() % 4) {
        case 0:
        case 1: {
            const Side side = rng() % 2 ? Side::Buy : Side::Sell;
            const auto price = 90 + static_cast<std::int64_t>(rng() % 20);
            input.push_back(add(ts, next_id++, side, price, 1 + rng() % 500));
            break;
        }
        case 2:
            input.push_back(event(ts, T::Execute, id, 1 + rng() % 100));
            break;
        default:
            input.push_back(event(ts, T::Delete, id));
        }
    }

    const ReplayOptions options{.check_invariants_every = 100};
    VectorSource first_source{input};
    VectorSource second_source{input};
    MarketBook first_book{kSymbol, 8'192};
    MarketBook second_book{kSymbol, 8'192};

    const ReplayReport first = MarketReplay::run(first_source, first_book, options);
    const ReplayReport second = MarketReplay::run(second_source, second_book, options);

    EXPECT_TRUE(first.succeeded());
    EXPECT_GT(first_book.order_count(), 0u);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first_book.digest(), second_book.digest());
}

} // namespace
} // namespace simulator
