#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "simulator/market/market_books.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/market/vector_source.hpp"
#include "simulator/metrics/latency_histogram.hpp"

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

MarketEvent status(Timestamp ts, TradingState state)
{
    return MarketEvent{.timestamp = ts, .symbol = kSymbol, .type = MarketEventType::Status, .trading_state = state};
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

// Halted, the book crosses at the open and passes every check; the Trading
// status makes the same crossed book a violation.
TEST(MarketReplayTest, HaltedBookMayCrossInRthUntilTradingResumes)
{
    constexpr Timestamp midnight = 1'577'682'000'000'000'000;
    constexpr Timestamp open = midnight + RTH_OPEN_NS;

    const std::vector<MarketEvent> events{
        status(open, TradingState::Halted), add(open, 1, Side::Buy, 101, 10), add(open, 2, Side::Sell, 100, 10),
        status(open, TradingState::Trading)};
    const ReplayOptions options{.check_invariants_every = 1, .session_midnight = midnight};

    VectorSource source{events};
    MarketBook book{kSymbol, 4};
    const ReplayReport report = MarketReplay::run(source, book, options);

    EXPECT_EQ(report.failure, ReplayFailure::InvariantViolation);
    EXPECT_EQ(report.invariant_error, InvariantError::CrossedInRth);
    EXPECT_EQ(report.failed_position, 4u);
    EXPECT_EQ(report.invariant_checks, 4u);
    EXPECT_EQ(by_type(report, MarketEventType::Status), 2u);
    EXPECT_EQ(report.applied(), 4u);

    EXPECT_EQ(to_string(MarketEventType::Status), "Status");
    EXPECT_EQ(to_string(TradingState::QuotationOnly), "QuotationOnly");
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

TEST(MarketReplayTest, LatencyRecordsOneSamplePerFullBatchAndChangesNothingElse)
{
    std::vector<MarketEvent> input;
    for (OrderId id = 1; id <= 10; ++id) {
        input.push_back(add(static_cast<Timestamp>(id), id, Side::Buy, 100, 10));
    }
    input.push_back(add(11, 1, Side::Buy, 100, 10));  // rejected, still an event of its batch

    VectorSource plain_source{input};
    VectorSource timed_source{input};
    MarketBook plain_book{kSymbol, 16};
    MarketBook timed_book{kSymbol, 16};
    LatencyHistogram latency;

    const ReplayReport plain = MarketReplay::run(plain_source, plain_book, {});
    const ReplayReport timed =
        MarketReplay::run(timed_source, timed_book, {.latency = &latency, .latency_batch = 4});

    EXPECT_EQ(latency.count(), 2u);  // events 1-4 and 5-8; 9-11 are a partial batch
    EXPECT_EQ(timed, plain);
    EXPECT_EQ(timed_book.digest(), plain_book.digest());
}

// `count` instruments, SymbolIds 0 .. count - 1.
InstrumentRegistry registry_of(std::size_t count)
{
    InstrumentRegistry registry{count};
    for (std::size_t i = 0; i < count; ++i) {
        Instrument instrument{.venue = {"XNAS"}};
        instrument.native_symbol[0] = static_cast<char>('A' + i);
        SymbolId id{};
        registry.add(instrument, id);
    }
    return registry;
}

MarketEvent on(SymbolId symbol, MarketEvent e)
{
    e.symbol = symbol;
    return e;
}

std::uint64_t by_result(const SymbolReport& report, ApplyResult result)
{
    return report.by_result[static_cast<std::size_t>(result)];
}

TEST(MarketReplayBooksTest, RoutesEachEventToTheBookOfItsSymbol)
{
    using T = MarketEventType;
    const InstrumentRegistry registry = registry_of(3);
    const std::vector<MarketEvent> input{
        on(0, add(1, 1, Side::Buy, 100, 10)),
        on(2, add(2, 1, Side::Sell, 200, 5)),  // the same id in another book
        on(2, add(3, 2, Side::Sell, 201, 5)),
        on(0, event(4, T::Execute, 1, 4)),
        on(2, event(5, T::Delete, 1)),
        on(2, event(6, T::Delete, 7)),           // UnknownOrder
        on(9, add(7, 3, Side::Buy, 100, 10))};  // not registered: WrongSymbol

    OrderPool pool{8};
    MarketBooks books{registry, pool};
    VectorSource source{input};
    const BooksReplayReport report = MarketReplay::run(source, books, {});

    EXPECT_TRUE(report.total.succeeded());
    EXPECT_EQ(report.total.events, 7u);
    EXPECT_EQ(report.total.applied(), 5u);
    EXPECT_EQ(by_result(report.total, ApplyResult::UnknownOrder), 1u);
    EXPECT_EQ(by_result(report.total, ApplyResult::WrongSymbol), 1u);
    EXPECT_EQ(by_type(report.total, T::Add), 4u);
    EXPECT_EQ(report.total.last_timestamp, 7);

    ASSERT_EQ(books.size(), 3u);  // one per registered instrument, with events or not
    ASSERT_EQ(report.symbols.size(), 3u);

    const SymbolReport& a = report.symbols[0];
    EXPECT_EQ(a.events, 2u);
    EXPECT_EQ(a.applied(), 2u);
    EXPECT_EQ(a.peak_orders, 1u);
    EXPECT_EQ(a.orders_left, 1u);

    EXPECT_EQ(report.symbols[1], SymbolReport{});

    const SymbolReport& c = report.symbols[2];
    EXPECT_EQ(c.events, 4u);
    EXPECT_EQ(c.applied(), 3u);
    EXPECT_EQ(c.rejected(), 1u);
    EXPECT_EQ(by_result(c, ApplyResult::UnknownOrder), 1u);
    EXPECT_EQ(c.peak_orders, 2u);
    EXPECT_EQ(c.orders_left, 1u);

    EXPECT_EQ(books[0].quantity_at(Side::Buy, Price{100}), 6u);
    EXPECT_EQ(books[2].quantity_at(Side::Sell, Price{201}), 5u);
    EXPECT_EQ(report.peak_live_orders, 3u);  // after the third Add
    EXPECT_EQ(pool.in_use(), 2u);

    InvariantError error{InvariantError::None};
    SymbolId failed{0};
    EXPECT_TRUE(books.check_invariants(error, failed)) << to_string(error);
}

TEST(MarketReplayBooksTest, AFullSharedPoolRejectsAddsAsPoolExhausted)
{
    const InstrumentRegistry registry = registry_of(2);
    const std::vector<MarketEvent> input{
        on(0, add(1, 1, Side::Buy, 100, 10)),
        on(1, add(2, 1, Side::Buy, 100, 10)),
        on(1, add(3, 2, Side::Buy, 100, 10)),  // no free slot
        on(0, event(4, MarketEventType::Delete, 1)),
        on(1, add(5, 2, Side::Buy, 100, 10))};

    OrderPool pool{2};
    MarketBooks books{registry, pool};
    VectorSource source{input};
    const BooksReplayReport report = MarketReplay::run(source, books, {});

    EXPECT_TRUE(report.total.succeeded());
    EXPECT_EQ(report.total.applied(), 4u);
    EXPECT_EQ(by_result(report.total, ApplyResult::PoolExhausted), 1u);
    EXPECT_EQ(by_result(report.symbols[1], ApplyResult::PoolExhausted), 1u);
    EXPECT_EQ(report.symbols[1].orders_left, 2u);
    EXPECT_EQ(report.peak_live_orders, 2u);
}

TEST(MarketReplayBooksTest, ChecksTheTouchedBookEveryNAppliedEventsAndNamesTheFailingSymbol)
{
    constexpr Timestamp midnight = 1'577'682'000'000'000'000;  // 2019-12-30, -05:00
    constexpr Timestamp open = midnight + RTH_OPEN_NS;
    const InstrumentRegistry registry = registry_of(2);
    const std::vector<MarketEvent> input{
        on(0, add(open, 1, Side::Buy, 101, 10)),
        on(0, add(open, 2, Side::Sell, 102, 10)),
        on(1, add(open, 1, Side::Buy, 101, 10)),
        on(1, add(open, 2, Side::Sell, 100, 10)),  // crosses book 1 in RTH
        on(0, add(open, 3, Side::Sell, 103, 10))};

    OrderPool pool{8};
    MarketBooks books{registry, pool};
    VectorSource source{input};
    const BooksReplayReport report =
        MarketReplay::run(source, books, {.check_invariants_every = 1, .session_midnight = midnight});

    EXPECT_EQ(report.total.failure, ReplayFailure::InvariantViolation);
    EXPECT_EQ(report.total.invariant_error, InvariantError::CrossedInRth);
    EXPECT_EQ(report.failed_symbol, 1u);
    EXPECT_EQ(report.total.invariant_checks, 4u);
    EXPECT_EQ(report.total.events, 4u);
}

TEST(MarketReplayBooksTest, InvariantsCompareTheSharedPoolWithAllBooks)
{
    const InstrumentRegistry registry = registry_of(2);
    OrderPool pool{4};
    MarketBooks books{registry, pool};
    ASSERT_NE(books.find(1), nullptr);
    ASSERT_EQ(books.find(1)->apply(on(1, add(1, 1, Side::Buy, 100, 10))), ApplyResult::Applied);
    EXPECT_EQ(books.find(5), nullptr);

    InvariantError error{InvariantError::None};
    SymbolId failed{0};
    EXPECT_TRUE(books.check_invariants(error, failed));

    MarketBook stray{7, pool};  // holds a slot no book in `books` accounts for
    ASSERT_EQ(stray.apply(on(7, add(1, 1, Side::Buy, 100, 10))), ApplyResult::Applied);
    EXPECT_FALSE(books.check_invariants(error, failed));
    EXPECT_EQ(error, InvariantError::PoolCountMismatch);
}

TEST(MarketReplayBooksTest, LatencyRecordsOneSamplePerFullBatch)
{
    const InstrumentRegistry registry = registry_of(2);
    std::vector<MarketEvent> input;
    for (OrderId id = 1; id <= 9; ++id) {
        input.push_back(on(static_cast<SymbolId>(id % 2), add(static_cast<Timestamp>(id), id, Side::Sell, 100, 10)));
    }

    OrderPool pool{16};
    MarketBooks books{registry, pool};
    VectorSource source{input};
    LatencyHistogram latency;
    const BooksReplayReport report = MarketReplay::run(source, books, {.latency = &latency, .latency_batch = 3});

    EXPECT_EQ(report.total.applied(), 9u);
    EXPECT_EQ(latency.count(), 3u);
}

TEST(MarketReplayBooksTest, CombinedDigestIsStableAndChangesWithAnyBook)
{
    using T = MarketEventType;
    const InstrumentRegistry registry = registry_of(3);
    std::mt19937_64 rng{7};
    std::vector<MarketEvent> input;
    OrderId next_id = 1;

    for (Timestamp ts = 0; ts < 3'000; ++ts) {
        const auto symbol = static_cast<SymbolId>(rng() % 3);
        const OrderId id = 1 + rng() % next_id;
        if (rng() % 2 == 0) {
            const Side side = rng() % 2 ? Side::Buy : Side::Sell;
            input.push_back(on(symbol, add(ts, next_id++, side, 90 + static_cast<std::int64_t>(rng() % 20),
                                           1 + rng() % 500)));
        } else {
            input.push_back(on(symbol, event(ts, rng() % 2 ? T::Execute : T::Delete, id, 1 + rng() % 100)));
        }
    }

    const ReplayOptions options{.check_invariants_every = 50};
    auto replay = [&](const std::vector<MarketEvent>& events, BooksReplayReport& report) {
        OrderPool pool{4'096};
        MarketBooks books{registry, pool};
        VectorSource source{events};
        report = MarketReplay::run(source, books, options);
        return books.digest();
    };

    BooksReplayReport first;
    BooksReplayReport second;
    const std::uint64_t digest = replay(input, first);
    EXPECT_TRUE(first.total.succeeded());
    EXPECT_GT(first.total.rejected(), 0u);  // unknown ids, too-large executes: fine
    EXPECT_EQ(replay(input, second), digest);
    EXPECT_EQ(second, first);

    // One more share resting in book 1, then the same share in book 2 instead.
    BooksReplayReport ignored;
    std::vector<MarketEvent> changed = input;
    changed.push_back(on(1, add(3'000, next_id, Side::Buy, 50, 1)));
    const std::uint64_t in_book_1 = replay(changed, ignored);
    changed.back().symbol = 2;
    const std::uint64_t in_book_2 = replay(changed, ignored);

    EXPECT_NE(in_book_1, digest);
    EXPECT_NE(in_book_2, digest);
    EXPECT_NE(in_book_1, in_book_2);
}

} // namespace
} // namespace simulator
