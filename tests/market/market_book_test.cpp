#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "simulator/market/market_book.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 7;

MarketEvent add(OrderId id, Side side, std::int64_t price, std::uint64_t quantity)
{
    return MarketEvent{.order_id = id,
                       .price = Price{price},
                       .quantity = Quantity{quantity},
                       .symbol = kSymbol,
                       .type = MarketEventType::Add,
                       .side = side};
}

MarketEvent reduce(MarketEventType type, OrderId id, std::uint64_t quantity)
{
    return MarketEvent{.order_id = id, .quantity = Quantity{quantity}, .symbol = kSymbol, .type = type};
}

MarketEvent execute(OrderId id, std::uint64_t quantity)
{
    return reduce(MarketEventType::Execute, id, quantity);
}
MarketEvent cancel(OrderId id, std::uint64_t quantity)
{
    return reduce(MarketEventType::Cancel, id, quantity);
}
MarketEvent erase(OrderId id)
{
    return reduce(MarketEventType::Delete, id, 0);
}

MarketEvent replace(OrderId id, OrderId new_id, std::int64_t price, std::uint64_t quantity)
{
    return MarketEvent{.order_id = id,
                       .new_order_id = new_id,
                       .price = Price{price},
                       .quantity = Quantity{quantity},
                       .symbol = kSymbol,
                       .type = MarketEventType::Replace};
}

MarketEvent status(TradingState state)
{
    return MarketEvent{.symbol = kSymbol, .type = MarketEventType::Status, .trading_state = state};
}

void expect_invariants(const MarketBook& book)
{
    InvariantError error{InvariantError::None};
    EXPECT_TRUE(book.check_invariants(error));
    EXPECT_EQ(error, InvariantError::None);
}

TEST(MarketBookTest, AppliesHistoricalOrderLifecycle)
{
    MarketBook book{kSymbol, 16};

    ASSERT_EQ(book.apply(add(100, Side::Buy, 100, 1000)), ApplyResult::Applied);
    const Order* order = book.find(100);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->remaining_quantity(), Quantity{1000});
    EXPECT_EQ(order->state(), OrderState::Active);

    ASSERT_EQ(book.apply(execute(100, 300)), ApplyResult::Applied);
    EXPECT_EQ(order->remaining_quantity(), Quantity{700});
    EXPECT_EQ(order->state(), OrderState::PartiallyFilled);
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), 700u);

    ASSERT_EQ(book.apply(cancel(100, 200)), ApplyResult::Applied);
    EXPECT_EQ(order->remaining_quantity(), Quantity{500});
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), 500u);
    expect_invariants(book);

    ASSERT_EQ(book.apply(erase(100)), ApplyResult::Applied);
    EXPECT_EQ(book.find(100), nullptr);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), 0u);
    expect_invariants(book);
}

TEST(MarketBookTest, FullExecuteRemovesOrderAndFreesItsSlot)
{
    MarketBook book{kSymbol, 1};

    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 100)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(execute(1, 100)), ApplyResult::Applied);
    EXPECT_EQ(book.find(1), nullptr);
    expect_invariants(book);

    // Capacity 1: the slot came back to the pool.
    EXPECT_EQ(book.apply(add(2, Side::Sell, 101, 10)), ApplyResult::Applied);
}

TEST(MarketBookTest, FullCancelRemovesOrderAndFreesItsSlot)
{
    MarketBook book{kSymbol, 1};

    ASSERT_EQ(book.apply(add(1, Side::Sell, 100, 100)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(cancel(1, 40)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(cancel(1, 60)), ApplyResult::Applied);
    EXPECT_EQ(book.find(1), nullptr);
    expect_invariants(book);
    EXPECT_EQ(book.apply(add(2, Side::Sell, 101, 10)), ApplyResult::Applied);
}

TEST(MarketBookTest, AddAssignsIncreasingSequencesAndTracksPeak)
{
    MarketBook book{kSymbol, 8};
    EXPECT_EQ(book.last_sequence(), 0u);

    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Buy, 100, 10)), ApplyResult::Applied);
    EXPECT_EQ(book.find(1)->sequence(), 1u);
    EXPECT_EQ(book.find(2)->sequence(), 2u);
    EXPECT_EQ(book.last_sequence(), 2u);

    ASSERT_EQ(book.apply(erase(1)), ApplyResult::Applied);
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.peak_order_count(), 2u);
}

TEST(MarketBookTest, ReplaceGoesToBackOfQueue)
{
    MarketBook book{kSymbol, 8};
    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Buy, 101, 20)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(3, Side::Buy, 101, 30)), ApplyResult::Applied);

    // Order 2 (head of 101) is replaced at the same price: it loses priority.
    ASSERT_EQ(book.apply(replace(2, 20, 101, 25)), ApplyResult::Applied);
    EXPECT_EQ(book.find(2), nullptr);

    const Order* replacement = book.find(20);
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->side(), Side::Buy);  // inherited
    EXPECT_EQ(replacement->price(), Price{101});
    EXPECT_EQ(replacement->remaining_quantity(), Quantity{25});
    EXPECT_EQ(replacement->sequence(), 4u);
    EXPECT_EQ(book.find(3)->next(), replacement);
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{101}), 55u);

    // A price change moves it to the back of the new level.
    ASSERT_EQ(book.apply(replace(20, 21, 100, 5)), ApplyResult::Applied);
    EXPECT_EQ(book.find(1)->next(), book.find(21));
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), 15u);
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{101}), 30u);
    EXPECT_EQ(book.order_count(), 3u);
    expect_invariants(book);
}

TEST(MarketBookTest, RejectedReplaceLeavesBookUnchanged)
{
    MarketBook book{kSymbol, 2};
    ASSERT_EQ(book.apply(add(1, Side::Sell, 100, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Sell, 100, 20)), ApplyResult::Applied);
    const std::uint64_t before = book.digest();

    EXPECT_EQ(book.apply(replace(1, 3, 101, 0)), ApplyResult::ZeroQuantity);
    EXPECT_EQ(book.apply(replace(1, 1, 101, 5)), ApplyResult::InvalidReplace);
    EXPECT_EQ(book.apply(replace(1, 2, 101, 5)), ApplyResult::DuplicateOrder);
    EXPECT_EQ(book.apply(replace(9, 3, 101, 5)), ApplyResult::UnknownOrder);

    EXPECT_EQ(book.digest(), before);
    EXPECT_EQ(book.find(1)->remaining_quantity(), Quantity{10});
    EXPECT_EQ(book.last_sequence(), 2u);
    expect_invariants(book);

    // A full pool still replaces: the old slot is freed first.
    EXPECT_EQ(book.apply(replace(1, 3, 101, 5)), ApplyResult::Applied);
    expect_invariants(book);
}

TEST(MarketBookTest, RejectsEachInvalidEventWithItsOwnResult)
{
    MarketBook book{kSymbol, 1};
    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
    const std::uint64_t before = book.digest();

    MarketEvent other_symbol = add(5, Side::Buy, 100, 10);
    other_symbol.symbol = kSymbol + 1;
    EXPECT_EQ(book.apply(other_symbol), ApplyResult::WrongSymbol);

    EXPECT_EQ(book.apply(execute(9, 1)), ApplyResult::UnknownOrder);
    EXPECT_EQ(book.apply(cancel(9, 1)), ApplyResult::UnknownOrder);
    EXPECT_EQ(book.apply(erase(9)), ApplyResult::UnknownOrder);

    EXPECT_EQ(book.apply(add(1, Side::Sell, 101, 10)), ApplyResult::DuplicateOrder);

    EXPECT_EQ(book.apply(add(2, Side::Buy, 100, 0)), ApplyResult::ZeroQuantity);
    EXPECT_EQ(book.apply(execute(1, 0)), ApplyResult::ZeroQuantity);
    EXPECT_EQ(book.apply(cancel(1, 0)), ApplyResult::ZeroQuantity);

    EXPECT_EQ(book.apply(execute(1, 11)), ApplyResult::ExceedsRemaining);
    EXPECT_EQ(book.apply(cancel(1, 11)), ApplyResult::ExceedsRemaining);

    EXPECT_EQ(book.apply(add(2, Side::Buy, 100, 10)), ApplyResult::CapacityExhausted);
    EXPECT_EQ(book.exhausted(), BookStructure::OrderPool);

    EXPECT_EQ(book.apply(reduce(MarketEventType::Modify, 1, 5)), ApplyResult::Unsupported);

    EXPECT_EQ(book.digest(), before);
    EXPECT_EQ(book.order_count(), 1u);
    expect_invariants(book);
}

TEST(MarketBookTest, TradeIsAppliedWithoutTouchingTheBook)
{
    MarketBook book{kSymbol, 4};
    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
    const std::uint64_t before = book.digest();

    MarketEvent trade{.order_id = 1,
                      .price = Price{100},
                      .quantity = Quantity{10},
                      .symbol = kSymbol,
                      .type = MarketEventType::Trade};
    EXPECT_EQ(book.apply(trade), ApplyResult::Applied);

    EXPECT_EQ(book.digest(), before);
    EXPECT_EQ(book.find(1)->remaining_quantity(), Quantity{10});
    EXPECT_EQ(book.last_sequence(), 1u);
}

TEST(MarketBookTest, StatusSetsTheTradingStateWithoutTouchingOrders)
{
    MarketBook book{kSymbol, 4};
    EXPECT_EQ(book.trading_state(), TradingState::Trading);
    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
    const std::uint64_t before = book.digest();

    for (const TradingState state :
         {TradingState::Halted, TradingState::Paused, TradingState::QuotationOnly, TradingState::Trading}) {
        EXPECT_EQ(book.apply(status(state)), ApplyResult::Applied);
        EXPECT_EQ(book.trading_state(), state);
    }

    MarketEvent other_symbol = status(TradingState::Halted);
    other_symbol.symbol = kSymbol + 1;
    EXPECT_EQ(book.apply(other_symbol), ApplyResult::WrongSymbol);
    EXPECT_EQ(book.trading_state(), TradingState::Trading);

    EXPECT_EQ(book.digest(), before);
    EXPECT_EQ(book.last_sequence(), 1u);
    EXPECT_EQ(book.order_count(), 1u);
}

// Every order leaves and frees its slot, the book trades again and sequences restart at 1, as in a fresh book.
TEST(MarketBookTest, ClearResetsTheBookToAFreshOne)
{
    MarketBook book{kSymbol, 3};
    for (const MarketEvent& event : {add(1, Side::Buy, 100, 10), add(2, Side::Sell, 101, 20), add(3, Side::Buy, 99, 30),
                                     status(TradingState::Halted)}) {
        ASSERT_EQ(book.apply(event), ApplyResult::Applied);
    }
    const MarketEvent clear{.symbol = kSymbol, .type = MarketEventType::Clear};
    MarketEvent other_symbol = clear;
    other_symbol.symbol = kSymbol + 1;
    EXPECT_EQ(book.apply(other_symbol), ApplyResult::WrongSymbol);
    EXPECT_EQ(book.order_count(), 3u);

    ASSERT_EQ(book.apply(clear), ApplyResult::Applied);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.trading_state(), TradingState::Trading);
    EXPECT_EQ(book.last_sequence(), 0u);
    EXPECT_EQ(book.peak_order_count(), 3u);  // kept
    expect_invariants(book);                 // no slot left in use

    MarketBook fresh{kSymbol, 3};
    EXPECT_EQ(book.digest(), fresh.digest());
    for (const MarketEvent& event :
         {add(7, Side::Buy, 100, 5), add(8, Side::Buy, 100, 6), add(9, Side::Sell, 102, 7)}) {
        ASSERT_EQ(book.apply(event), ApplyResult::Applied);  // every slot is free again
        ASSERT_EQ(fresh.apply(event), ApplyResult::Applied);
    }
    EXPECT_EQ(book.find(7)->sequence(), 1u);
    EXPECT_EQ(book.digest(), fresh.digest());
}

TEST(MarketBookTest, BestIsFalseOnAnEmptySide)
{
    MarketBook book{kSymbol, 4};
    LevelInfo level{};
    EXPECT_FALSE(book.best(Side::Buy, level));
    EXPECT_FALSE(book.best(Side::Sell, level));

    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
    EXPECT_TRUE(book.best(Side::Buy, level));
    EXPECT_FALSE(book.best(Side::Sell, level));
}

TEST(MarketBookTest, BestReturnsTopLevelPriceSizeAndCount)
{
    MarketBook book{kSymbol, 8};
    ASSERT_EQ(book.apply(add(1, Side::Buy, 99, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Buy, 100, 20)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(3, Side::Buy, 100, 5)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(4, Side::Sell, 102, 7)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(5, Side::Sell, 101, 8)), ApplyResult::Applied);

    LevelInfo level{};
    ASSERT_TRUE(book.best(Side::Buy, level));
    EXPECT_EQ(level, (LevelInfo{Price{100}, 25, 2}));
    ASSERT_TRUE(book.best(Side::Sell, level));
    EXPECT_EQ(level, (LevelInfo{Price{101}, 8, 1}));
}

TEST(MarketBookTest, DepthCopiesLevelsBestFirstIntoTheCallersBuffer)
{
    MarketBook book{kSymbol, 8};
    ASSERT_EQ(book.apply(add(1, Side::Sell, 103, 1)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Sell, 101, 2)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(3, Side::Sell, 102, 3)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(4, Side::Buy, 99, 4)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(5, Side::Buy, 100, 5)), ApplyResult::Applied);

    std::array<LevelInfo, 2> top{};
    ASSERT_EQ(book.depth(Side::Sell, top), 2u);
    EXPECT_EQ(top[0], (LevelInfo{Price{101}, 2, 1}));
    EXPECT_EQ(top[1], (LevelInfo{Price{102}, 3, 1}));

    std::array<LevelInfo, 5> all{};
    ASSERT_EQ(book.depth(Side::Buy, all), 2u);
    EXPECT_EQ(all[0], (LevelInfo{Price{100}, 5, 1}));
    EXPECT_EQ(all[1], (LevelInfo{Price{99}, 4, 1}));

    MarketBook empty{kSymbol, 1};
    EXPECT_EQ(empty.depth(Side::Buy, all), 0u);
}

TEST(MarketBookTest, QuantityAtUsesTheLevelTotal)
{
    MarketBook book{kSymbol, 8};
    ASSERT_EQ(book.apply(add(1, Side::Sell, 101, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Sell, 101, 15)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(execute(1, 4)), ApplyResult::Applied);

    EXPECT_EQ(book.quantity_at(Side::Sell, Price{101}), 21u);
    EXPECT_EQ(book.quantity_at(Side::Sell, Price{102}), 0u);
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{101}), 0u);
}

TEST(MarketBookTest, VisitLevelsStopsWhenTheVisitorReturnsFalse)
{
    MarketBook book{kSymbol, 8};
    ASSERT_EQ(book.apply(add(1, Side::Buy, 98, 1)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Buy, 100, 1)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(3, Side::Buy, 99, 1)), ApplyResult::Applied);

    std::array<std::int64_t, 3> seen{};
    std::size_t visited = 0;
    book.visit_levels(Side::Buy, [&](const LevelInfo& level) {
        seen[visited++] = level.price.ticks;
        return visited < 2;
    });

    EXPECT_EQ(visited, 2u);
    EXPECT_EQ(seen[0], 100);
    EXPECT_EQ(seen[1], 99);
}

TEST(MarketBookTest, LockedIsNotCrossed)
{
    MarketBook book{kSymbol, 8};
    EXPECT_FALSE(book.is_locked());
    EXPECT_FALSE(book.is_crossed());

    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Sell, 100, 10)), ApplyResult::Applied);
    EXPECT_TRUE(book.is_locked());
    EXPECT_FALSE(book.is_crossed());

    ASSERT_EQ(book.apply(add(3, Side::Buy, 101, 10)), ApplyResult::Applied);
    EXPECT_FALSE(book.is_locked());
    EXPECT_TRUE(book.is_crossed());
}

// The book never hands out a mutable Order; this const_cast stands in for a
// bug inside it that changes an order behind the book's back.
Order& mutable_order(const MarketBook& book, OrderId id)
{
    return const_cast<Order&>(*book.find(id));
}

TEST(MarketBookTest, InvariantsDetectLevelTotalMismatch)
{
    MarketBook book{kSymbol, 4};
    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
    expect_invariants(book);

    // Executing the order directly skips PriceLevel::reduce.
    ASSERT_TRUE(mutable_order(book, 1).execute(Quantity{3}));

    InvariantError error{InvariantError::None};
    EXPECT_FALSE(book.check_invariants(error));
    EXPECT_EQ(error, InvariantError::LevelTotalMismatch);
}

TEST(MarketBookTest, InvariantsDetectSequenceNotIncreasing)
{
    MarketBook book{kSymbol, 4};
    ASSERT_EQ(book.apply(add(1, Side::Sell, 100, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Sell, 100, 10)), ApplyResult::Applied);

    mutable_order(book, 2).set_sequence(1);

    InvariantError error{InvariantError::None};
    EXPECT_FALSE(book.check_invariants(error));
    EXPECT_EQ(error, InvariantError::SequenceNotIncreasing);
}

TEST(MarketBookTest, InvariantsDetectInactiveOrderInBook)
{
    MarketBook book{kSymbol, 4};
    ASSERT_EQ(book.apply(add(1, Side::Sell, 100, 10)), ApplyResult::Applied);

    // Order::cancel() changes state without unlinking.
    ASSERT_TRUE(mutable_order(book, 1).cancel());

    InvariantError error{InvariantError::None};
    EXPECT_FALSE(book.check_invariants(error));
    EXPECT_EQ(error, InvariantError::InactiveOrderInBook);
}

TEST(MarketBookTest, CrossedBookFailsInvariantsOnlyDuringRth)
{
    MarketBook book{kSymbol, 4};
    ASSERT_EQ(book.apply(add(1, Side::Buy, 101, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Sell, 100, 10)), ApplyResult::Applied);

    InvariantError error{InvariantError::None};
    EXPECT_TRUE(book.check_invariants(error, false));  // pre-market: allowed

    EXPECT_FALSE(book.check_invariants(error, true));
    EXPECT_EQ(error, InvariantError::CrossedInRth);
}

// A book that is not Trading queues orders without matching them, so it may cross.
TEST(MarketBookTest, CrossedBookFailsInRthOnlyWhileTrading)
{
    MarketBook book{kSymbol, 4};
    ASSERT_EQ(book.apply(add(1, Side::Buy, 101, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(add(2, Side::Sell, 100, 10)), ApplyResult::Applied);

    InvariantError error{InvariantError::None};
    for (const TradingState state : {TradingState::Halted, TradingState::Paused, TradingState::QuotationOnly}) {
        ASSERT_EQ(book.apply(status(state)), ApplyResult::Applied);
        EXPECT_TRUE(book.check_invariants(error, true));
        EXPECT_EQ(error, InvariantError::None);
    }

    ASSERT_EQ(book.apply(status(TradingState::Trading)), ApplyResult::Applied);
    EXPECT_FALSE(book.check_invariants(error, true));
    EXPECT_EQ(error, InvariantError::CrossedInRth);
}

TEST(MarketBookTest, NotTradingStillChecksTheBookStructure)
{
    MarketBook book{kSymbol, 4};
    ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
    ASSERT_EQ(book.apply(status(TradingState::Halted)), ApplyResult::Applied);

    ASSERT_TRUE(mutable_order(book, 1).execute(Quantity{3}));

    InvariantError error{InvariantError::None};
    EXPECT_FALSE(book.check_invariants(error, true));
    EXPECT_EQ(error, InvariantError::LevelTotalMismatch);
}

TEST(MarketBookTest, InRthIsHalfOpenFromOpenToClose)
{
    constexpr Timestamp midnight = 1'000'000'000;
    EXPECT_FALSE(in_rth(midnight + RTH_OPEN_NS - 1, midnight));
    EXPECT_TRUE(in_rth(midnight + RTH_OPEN_NS, midnight));
    EXPECT_TRUE(in_rth(midnight + RTH_CLOSE_NS - 1, midnight));
    EXPECT_FALSE(in_rth(midnight + RTH_CLOSE_NS, midnight));
}

TEST(MarketBookTest, DigestMatchesForSameEventsAndDiffersByOneShare)
{
    const MarketEvent events[] = {
        add(1, Side::Buy, 100, 10), add(2, Side::Sell, 101, 20), add(3, Side::Buy, 100, 30), execute(1, 4),
        replace(2, 4, 102, 5),
    };

    MarketBook a{kSymbol, 8};
    MarketBook b{kSymbol, 8};
    for (const MarketEvent& event : events) {
        ASSERT_EQ(a.apply(event), ApplyResult::Applied);
        ASSERT_EQ(b.apply(event), ApplyResult::Applied);
    }
    EXPECT_EQ(a.digest(), b.digest());
    EXPECT_NE(a.digest(), MarketBook(kSymbol, 8).digest());

    ASSERT_EQ(b.apply(cancel(3, 1)), ApplyResult::Applied);
    EXPECT_NE(a.digest(), b.digest());
}

// A fixed-seed stream of valid and invalid events of every type, with a Clear
// every 1,000 events so the pool still fills between them. Invariants hold
// after every apply, and a rejected event (or a Trade) never changes the
// digest. Returns the final digest.
std::uint64_t run_random_stream(std::uint64_t seed, std::array<std::size_t, apply_result_count>& counts)
{
    MarketBook book{kSymbol, 64};
    std::mt19937_64 rng{seed};

    for (int i = 0; i < 10'000; ++i) {
        static constexpr MarketEventType types[16] = {
            MarketEventType::Add,     MarketEventType::Add,    MarketEventType::Add,     MarketEventType::Add,
            MarketEventType::Add,     MarketEventType::Add,    MarketEventType::Execute, MarketEventType::Execute,
            MarketEventType::Cancel,  MarketEventType::Cancel, MarketEventType::Delete,  MarketEventType::Replace,
            MarketEventType::Replace, MarketEventType::Trade,  MarketEventType::Modify,  MarketEventType::Modify,
        };

        MarketEvent event{};
        event.timestamp = i;
        event.type = i % 1'000 == 999 ? MarketEventType::Clear : types[rng() % 16];
        event.order_id = 1 + rng() % 128;
        event.new_order_id = rng() % 8 == 0 ? event.order_id : 1 + rng() % 128;
        event.price = Price{static_cast<std::int64_t>(95 + rng() % 11)};
        event.quantity = Quantity{rng() % 8 == 0 ? 0 : 1 + rng() % 100};
        event.symbol = rng() % 64 == 0 ? kSymbol + 1 : kSymbol;
        event.side = rng() % 2 == 0 ? Side::Buy : Side::Sell;

        const std::uint64_t before = book.digest();
        const ApplyResult result = book.apply(event);
        ++counts[static_cast<std::size_t>(result)];

        if (result != ApplyResult::Applied || event.type == MarketEventType::Trade) {
            EXPECT_EQ(book.digest(), before) << "event " << i;
        }

        InvariantError error{InvariantError::None};
        EXPECT_TRUE(book.check_invariants(error)) << "event " << i << " error " << static_cast<int>(error);
    }

    return book.digest();
}

TEST(MarketBookTest, RandomStreamKeepsInvariantsAndIsDeterministic)
{
    std::array<std::size_t, apply_result_count> counts{};
    const std::uint64_t first = run_random_stream(42, counts);

    for (std::size_t r = 0; r < apply_result_count; ++r) {
        EXPECT_GT(counts[r], 0u) << "ApplyResult " << r << " never produced";
    }

    std::array<std::size_t, apply_result_count> again{};
    EXPECT_EQ(run_random_stream(42, again), first);
    EXPECT_EQ(again, counts);
}

MarketEvent on(SymbolId symbol, MarketEvent event)
{
    event.symbol = symbol;
    return event;
}

TEST(MarketBookTest, BooksOnASharedPoolDrawFromTheSameSlots)
{
    BookStorage storage{3};
    const OrderPool& pool = storage.orders;
    MarketBook a{1, storage};
    MarketBook b{2, storage};

    ASSERT_EQ(a.apply(on(1, add(1, Side::Buy, 100, 10))), ApplyResult::Applied);
    ASSERT_EQ(b.apply(on(2, add(1, Side::Sell, 101, 10))), ApplyResult::Applied);  // ids are per book
    ASSERT_EQ(a.apply(on(1, add(2, Side::Buy, 99, 10))), ApplyResult::Applied);
    EXPECT_EQ(pool.in_use(), 3u);

    EXPECT_EQ(b.apply(on(2, add(2, Side::Sell, 102, 10))), ApplyResult::CapacityExhausted);
    EXPECT_EQ(b.order_count(), 1u);
    EXPECT_EQ(b.apply(on(1, add(2, Side::Sell, 102, 10))), ApplyResult::WrongSymbol);

    // A slot one book frees, the other can use.
    ASSERT_EQ(a.apply(on(1, erase(1))), ApplyResult::Applied);
    EXPECT_EQ(storage.orders.in_use(), 2u);
    ASSERT_EQ(b.apply(on(2, add(2, Side::Sell, 102, 10))), ApplyResult::Applied);
    ASSERT_EQ(b.apply(on(2, replace(2, 3, 103, 5))), ApplyResult::Applied);  // full pool: frees, then takes
    ASSERT_EQ(b.apply(on(2, execute(1, 10))), ApplyResult::Applied);
    EXPECT_EQ(storage.orders.in_use(), 2u);
    EXPECT_EQ(a.order_count() + b.order_count(), pool.in_use());

    // A book on a shared pool cannot compare the pool with its own orders alone.
    expect_invariants(a);
    expect_invariants(b);
}

TEST(MarketBookTest, VisitOrdersWalksOneLevelFrontFirst)
{
    MarketBook book{kSymbol, 16};
    const auto add_at = [&](OrderId id, Side side, std::int64_t price) {
        ASSERT_EQ(book.apply(MarketEvent{.timestamp = static_cast<Timestamp>(id),
                                         .order_id = id,
                                         .price = Price{price},
                                         .quantity = Quantity{10},
                                         .symbol = kSymbol,
                                         .type = MarketEventType::Add,
                                         .side = side}),
                  ApplyResult::Applied);
    };
    add_at(1, Side::Buy, 100);
    add_at(2, Side::Buy, 100);
    add_at(3, Side::Buy, 100);
    add_at(4, Side::Buy, 99);
    add_at(5, Side::Sell, 101);

    std::vector<OrderId> ids;
    book.visit_orders(Side::Buy, Price{100}, [&](const Order& order) {
        ids.push_back(order.id());
        return true;
    });
    EXPECT_EQ(ids, (std::vector<OrderId>{1, 2, 3}));

    ids.clear();
    book.visit_orders(Side::Buy, Price{100}, [&](const Order& order) {
        ids.push_back(order.id());
        return ids.size() < 2;
    });
    EXPECT_EQ(ids, (std::vector<OrderId>{1, 2}));

    ids.clear();
    book.visit_orders(Side::Sell, Price{100}, [&](const Order& order) {
        ids.push_back(order.id());
        return true;
    });
    EXPECT_TRUE(ids.empty());
}

// Each structure, filled, refuses the Add or Replace that needs one more entry, names itself and leaves the book as it
// was; the storage still agrees with the book.
TEST(MarketBookTest, EachFullStructureRefusesWhatNeedsItAndLeavesTheBookIntact)
{
    auto expect_refused = [](MarketBook& book, const BookStorage& storage, const MarketEvent& event,
                             BookStructure full) {
        const std::uint64_t before = book.digest();
        const std::size_t orders = book.order_count();
        EXPECT_EQ(book.apply(event), ApplyResult::CapacityExhausted);
        EXPECT_EQ(book.exhausted(), full);
        EXPECT_EQ(book.digest(), before);
        EXPECT_EQ(book.order_count(), orders);
        expect_invariants(book);
        EXPECT_EQ(storage.orders.in_use(), book.order_count());
        EXPECT_EQ(storage.order_index.size(), book.order_count());
        EXPECT_EQ(storage.levels.in_use(), book.level_count());
        EXPECT_EQ(storage.level_index.size(), book.level_count());
    };

    {
        BookStorage storage{BookCapacity{.orders = 2, .order_index = 16, .levels = 16, .level_index = 16}};
        MarketBook book{kSymbol, storage};
        ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
        ASSERT_EQ(book.apply(add(2, Side::Buy, 100, 10)), ApplyResult::Applied);
        expect_refused(book, storage, add(3, Side::Buy, 100, 10), BookStructure::OrderPool);
        ASSERT_EQ(book.apply(replace(2, 3, 101, 10)), ApplyResult::Applied);  // frees a slot, then takes it
    }
    {
        BookStorage storage{BookCapacity{.orders = 16, .order_index = 8, .levels = 16, .level_index = 16}};
        MarketBook book{kSymbol, storage};
        for (OrderId id = 1; id <= 8; ++id) {
            ASSERT_EQ(book.apply(add(id, Side::Sell, 100, 10)), ApplyResult::Applied);
        }
        expect_refused(book, storage, add(9, Side::Sell, 100, 10), BookStructure::OrderIndex);
        ASSERT_EQ(book.apply(replace(8, 9, 100, 10)), ApplyResult::Applied);  // its entry goes first
    }
    {
        BookStorage storage{BookCapacity{.orders = 16, .order_index = 16, .levels = 2, .level_index = 16}};
        MarketBook book{kSymbol, storage};
        ASSERT_EQ(book.apply(add(1, Side::Buy, 100, 10)), ApplyResult::Applied);
        ASSERT_EQ(book.apply(add(2, Side::Buy, 100, 10)), ApplyResult::Applied);
        ASSERT_EQ(book.apply(add(3, Side::Sell, 105, 10)), ApplyResult::Applied);
        ASSERT_EQ(book.apply(add(4, Side::Buy, 100, 10)), ApplyResult::Applied);  // an existing level
        expect_refused(book, storage, add(5, Side::Buy, 99, 10), BookStructure::LevelPool);
        expect_refused(book, storage, replace(1, 5, 99, 10), BookStructure::LevelPool);  // 100 keeps its others
        ASSERT_EQ(book.apply(replace(3, 5, 106, 10)), ApplyResult::Applied);            // 105 empties first
        ASSERT_EQ(book.apply(replace(1, 6, 100, 10)), ApplyResult::Applied);            // to a level that exists
    }
    {
        BookStorage storage{BookCapacity{.orders = 32, .order_index = 32, .levels = 32, .level_index = 8}};
        MarketBook book{kSymbol, storage};
        for (OrderId id = 1; id <= 8; ++id) {
            ASSERT_EQ(book.apply(add(id, Side::Buy, 100 - static_cast<std::int64_t>(id), 10)), ApplyResult::Applied);
        }
        expect_refused(book, storage, add(9, Side::Sell, 200, 10), BookStructure::LevelIndex);
        ASSERT_EQ(book.apply(add(9, Side::Buy, 95, 10)), ApplyResult::Applied);
    }
}

} // namespace
} // namespace simulator
