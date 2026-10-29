#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "simulator/strategies/orb.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 1;
constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};
constexpr Timestamp kMinute = 60'000'000'000;
constexpr Timestamp kOpen = 570 * kMinute;  // 09:30 of a day starting at 0
constexpr Timestamp kClose = 960 * kMinute;
constexpr Timestamp kRangeEnd = kOpen + 15 * kMinute;
constexpr Timestamp kFlatten = kClose - 5 * kMinute;
constexpr Timestamp kDay = 1'440 * kMinute;
constexpr std::int64_t kBid = 1'000'000;  // $100.00
constexpr std::int64_t kAsk = 1'000'100;  // $100.01
constexpr std::int64_t kHigh = 1'000'500;  // the range the tests trade: 99.98 to 100.05
constexpr std::int64_t kLow = 999'800;

static_assert(Strategy<Orb>);

// Drives an Orb directly: market callbacks with or without a trade, timers, and the reports of its orders.
struct Fixture
{
    std::vector<DaySession> sessions;
    MarketBook book{kSymbol, 64};
    SimQueue queue{64};
    Portfolio portfolio;
    OrderGateway gateway{queue, portfolio, LatencyConfig{.order_latency = 10}};
    StrategyContext ctx{book, portfolio, gateway};
    Orb orb;
    OrderId next_id{1};
    std::vector<Timestamp> timers;  // when each timer it set fires

    explicit Fixture(std::vector<DaySession> days = {{kOpen, kClose}}, std::size_t max_open_orders = 1)
        : sessions(std::move(days)),
          portfolio(kEquity,
                    RiskLimits{.max_position = 1'000,
                               .max_order_quantity = Quantity{1'000},
                               .max_open_orders = max_open_orders},
                    FeeSchedule{}),
          orb(OrbParams{.range_minutes = 15, .clip = 100}, sessions)
    {
        add(Side::Buy, kBid, 500);
        add(Side::Sell, kAsk, 500);
    }

    void add(Side side, std::int64_t price, std::uint64_t quantity)
    {
        ASSERT_EQ(book.apply(MarketEvent{.order_id = next_id++,
                                         .price = Price{price},
                                         .quantity = Quantity{quantity},
                                         .symbol = kSymbol,
                                         .side = side}),
                  ApplyResult::Applied);
    }

    void status(TradingState state)
    {
        ASSERT_EQ(book.apply(MarketEvent{.symbol = kSymbol, .type = MarketEventType::Status, .trading_state = state}),
                  ApplyResult::Applied);
    }

    // The orders queued since the last call; timers are kept in `timers` (the tests fire them by hand).
    std::vector<OrderRequest> sent()
    {
        std::vector<OrderRequest> orders;
        for (; !queue.empty(); queue.pop()) {
            if (queue.top().type == SimEventType::OrderArrive) {
                orders.push_back(queue.top().order);
            } else if (queue.top().type == SimEventType::Timer) {
                timers.push_back(queue.top().timestamp);
            }
        }
        return orders;
    }

    // A fill of `quantity` the strategy did not send, such as manual trades before it was attached.
    void inherit(Side side, std::uint64_t quantity)
    {
        ASSERT_EQ(portfolio.reserve(side, Quantity{quantity}), RiskCheck::Accepted);
        portfolio.on_report(ExecutionReport{.price = Price{side == Side::Buy ? kAsk : kBid},
                                            .quantity = Quantity{quantity},
                                            .type = ReportType::Fill,
                                            .liquidity = Liquidity::Taker,
                                            .side = side});
    }

    // A market callback at `at` that printed a trade at `price`.
    std::vector<OrderRequest> trade(Timestamp at, std::int64_t price)
    {
        gateway.set_now(at);
        const Trade print{.timestamp = at, .price = Price{price}, .quantity = 100, .side = Side::Buy};
        ctx.trade = &print;
        orb.on_market(ctx, MarketEvent{.timestamp = at});
        ctx.trade = nullptr;
        return sent();
    }

    // A market callback at `at` with no trade.
    std::vector<OrderRequest> event(Timestamp at)
    {
        gateway.set_now(at);
        orb.on_market(ctx, MarketEvent{.timestamp = at});
        return sent();
    }

    std::vector<OrderRequest> timer(Timestamp at)
    {
        gateway.set_now(at);
        orb.on_timer(ctx, 0);
        return sent();
    }

    // The order's last reports: `filled` shares, then the rest expired.
    void complete(const OrderRequest& order, std::uint64_t filled)
    {
        const std::uint64_t quantity = order.quantity.value();
        if (filled > 0) {
            report(ExecutionReport{.order_id = order.id,
                                   .price = order.price,
                                   .quantity = Quantity{filled},
                                   .leaves = Quantity{quantity - filled},
                                   .type = ReportType::Fill,
                                   .liquidity = Liquidity::Taker,
                                   .side = order.side});
        }
        if (filled < quantity) {
            report(ExecutionReport{.order_id = order.id,
                                   .quantity = Quantity{quantity - filled},
                                   .type = ReportType::Expired,
                                   .side = order.side});
        }
    }

    void report(const ExecutionReport& report)
    {
        portfolio.on_report(report);
        orb.on_report(ctx, report);
    }

    // The range of the tests: 99.98 to 100.05, from three trades inside [09:30, 09:45).
    void range()
    {
        ASSERT_TRUE(trade(kOpen, 1'000'200).empty());
        ASSERT_TRUE(trade(kOpen + 5 * kMinute, kHigh).empty());
        ASSERT_TRUE(trade(kRangeEnd - 1, kLow).empty());
    }

    // A long of `filled` shares, entered on a breakout at 10:00.
    void go_long(std::uint64_t filled = 100)
    {
        range();
        const std::vector<OrderRequest> entry = trade(kRangeEnd + 15 * kMinute, kHigh + 1);
        ASSERT_EQ(entry.size(), 1u);
        complete(entry[0], filled);
    }
};

void expect_order(const std::vector<OrderRequest>& orders, Side side, std::int64_t price, std::uint64_t quantity)
{
    ASSERT_EQ(orders.size(), 1u);
    EXPECT_EQ(orders[0].side, side);
    EXPECT_EQ(orders[0].price, Price{price});
    EXPECT_EQ(orders[0].quantity, Quantity{quantity});
    EXPECT_EQ(orders[0].time_in_force, TimeInForce::Ioc);
}

TEST(OrbTest, TheRangeIsTheHighAndLowOfTradesAloneInsideItsInterval)
{
    Fixture f;
    EXPECT_TRUE(f.trade(kOpen - 1, 2'000'000).empty());  // pre-market: not in the range
    f.range();
    EXPECT_TRUE(f.event(kOpen + 10 * kMinute).empty());  // a book change is not a trade
    EXPECT_TRUE(f.orb.stats().has_range);
    EXPECT_EQ(f.orb.stats().range_high, Price{kHigh});
    EXPECT_EQ(f.orb.stats().range_low, Price{kLow});
}

TEST(OrbTest, ATradeAtABoundaryIsNoBreakoutAndOneTickBeyondIs)
{
    Fixture f;
    f.range();
    EXPECT_TRUE(f.trade(kRangeEnd, kHigh).empty());
    EXPECT_TRUE(f.trade(kRangeEnd, kLow).empty());
    ASSERT_NO_FATAL_FAILURE(
        expect_order(f.trade(kRangeEnd + 1, kHigh + 1), Side::Buy, kAsk, 100));  // at the touch, not at the trade
}

TEST(OrbTest, TheRangeEndsAtItsDeadlineToTheNanosecond)
{
    Fixture f;
    f.range();
    EXPECT_TRUE(f.trade(kRangeEnd - 1, kHigh + 50).empty());  // still the range: its high rises
    EXPECT_EQ(f.orb.stats().range_high, Price{kHigh + 50});
    ASSERT_NO_FATAL_FAILURE(
        expect_order(f.trade(kRangeEnd, kHigh + 51), Side::Buy, kAsk, 100));  // the first eligible instant
}

TEST(OrbTest, AnEntryThatFillsInPartOrNotAtAllLeavesTheFilledPositionAndUsesItsDirection)
{
    Fixture part;
    part.go_long(40);
    EXPECT_EQ(part.portfolio.position(), 40);
    EXPECT_EQ(part.orb.stats().bought, 40u);
    EXPECT_EQ(part.orb.stats().expired, 60u);

    Fixture none;
    none.go_long(0);
    EXPECT_EQ(none.portfolio.position(), 0);
    EXPECT_EQ(none.orb.stats().long_entries, 1u);
    EXPECT_EQ(none.orb.stats().requested, 100u);
    EXPECT_TRUE(none.trade(kRangeEnd + 20 * kMinute, kHigh + 10).empty());  // the long is used
}

TEST(OrbTest, AStopThatFillsInPartRetriesUntilFlat)
{
    Fixture f;
    f.go_long();
    const std::vector<OrderRequest> stop = f.trade(kRangeEnd + 30 * kMinute, kLow - 1);
    ASSERT_NO_FATAL_FAILURE(expect_order(stop, Side::Sell, kBid, 100));
    f.complete(stop[0], 40);
    EXPECT_EQ(f.portfolio.position(), 60);

    const std::vector<OrderRequest> retry = f.event(kRangeEnd + 31 * kMinute);  // any market event retries
    ASSERT_NO_FATAL_FAILURE(expect_order(retry, Side::Sell, kBid, 60));
    f.complete(retry[0], 60);
    EXPECT_EQ(f.portfolio.position(), 0);
    EXPECT_TRUE(f.event(kRangeEnd + 32 * kMinute).empty());
    EXPECT_EQ(f.orb.stats().stops, 1u);
    EXPECT_EQ(f.orb.stats().closing_orders, 2u);
    EXPECT_EQ(f.orb.stats().sold, 100u);
}

TEST(OrbTest, TheFlattenTimerClosesWithoutAMarketEventAndRetriesAPartialFill)
{
    Fixture f;
    f.go_long();
    const std::vector<OrderRequest> flatten = f.timer(kFlatten);
    ASSERT_NO_FATAL_FAILURE(expect_order(flatten, Side::Sell, kBid, 100));
    f.complete(flatten[0], 30);
    const std::vector<OrderRequest> retry = f.event(kFlatten + 1);
    ASSERT_NO_FATAL_FAILURE(expect_order(retry, Side::Sell, kBid, 70));
    f.complete(retry[0], 70);
    EXPECT_EQ(f.portfolio.position(), 0);
    EXPECT_EQ(f.orb.stats().flattens, 1u);
    EXPECT_TRUE(f.event(kFlatten + 2).empty());  // flat: closing ends
    EXPECT_TRUE(f.trade(kFlatten + 3, kLow - 1).empty());  // the short is unused, but the deadline has passed
}

TEST(OrbTest, ItsFlattenTimerIsSetForFiveMinutesBeforeTheClose)
{
    Fixture f;
    EXPECT_TRUE(f.event(kOpen - kMinute).empty());
    EXPECT_EQ(f.timers, (std::vector<Timestamp>{kFlatten}));
}

TEST(OrbTest, AStoppedLongCannotReenterButTheShortStillCan)
{
    Fixture f;
    f.go_long();
    const std::vector<OrderRequest> stop = f.trade(kRangeEnd + 30 * kMinute, kLow - 1);
    f.complete(stop[0], 100);
    EXPECT_TRUE(f.trade(kRangeEnd + 40 * kMinute, kHigh + 1).empty());

    const std::vector<OrderRequest> entry = f.trade(kRangeEnd + 50 * kMinute, kLow - 1);  // a later trade
    ASSERT_NO_FATAL_FAILURE(expect_order(entry, Side::Sell, kBid, 100));
    f.complete(entry[0], 100);
    EXPECT_EQ(f.portfolio.position(), -100);
    const std::vector<OrderRequest> stop_short = f.trade(kRangeEnd + 60 * kMinute, kHigh + 1);
    ASSERT_NO_FATAL_FAILURE(expect_order(stop_short, Side::Buy, kAsk, 100));
    f.complete(stop_short[0], 100);
    EXPECT_EQ(f.portfolio.position(), 0);
    EXPECT_EQ(f.orb.stats().long_entries, 1u);
    EXPECT_EQ(f.orb.stats().short_entries, 1u);
    EXPECT_EQ(f.orb.stats().stops, 2u);
}

TEST(OrbTest, TheTradeThatStopsALongDoesNotAlsoEnterShort)
{
    Fixture f;
    f.go_long();
    const std::vector<OrderRequest> stop = f.trade(kRangeEnd + 30 * kMinute, kLow - 1);
    ASSERT_EQ(stop.size(), 1u);
    EXPECT_EQ(stop[0].quantity, Quantity{100});  // closes the long only
    EXPECT_EQ(f.orb.stats().short_entries, 0u);
}

TEST(OrbTest, AHaltHoldsThePositionUntilTradingResumesEvenAtTheFlatten)
{
    Fixture f;
    f.go_long();
    f.status(TradingState::Halted);
    EXPECT_TRUE(f.trade(kRangeEnd + 30 * kMinute, kLow - 1).empty());  // the stop, but no order while halted
    EXPECT_TRUE(f.timer(kFlatten).empty());
    f.status(TradingState::Trading);
    const std::vector<OrderRequest> close = f.event(kFlatten + kMinute);
    ASSERT_NO_FATAL_FAILURE(expect_order(close, Side::Sell, kBid, 100));
    f.complete(close[0], 100);
    EXPECT_EQ(f.portfolio.position(), 0);
    EXPECT_EQ(f.portfolio.open_orders(), 0u);
    EXPECT_EQ(f.orb.stats().stops, 1u);
    EXPECT_EQ(f.orb.stats().flattens, 0u);  // one closing process, begun by the stop
}

TEST(OrbTest, WithoutASessionItNeverTrades)
{
    Fixture f{{}};
    EXPECT_TRUE(f.trade(kOpen, kHigh).empty());
    EXPECT_TRUE(f.trade(kRangeEnd + kMinute, kHigh + 1'000).empty());
    EXPECT_TRUE(f.timer(kFlatten).empty());
    EXPECT_EQ(f.orb.stats(), OrbStats{});
}

TEST(OrbTest, ADayWithoutRangeTradesSendsNoEntry)
{
    Fixture f;
    EXPECT_TRUE(f.event(kOpen + kMinute).empty());
    EXPECT_TRUE(f.trade(kRangeEnd + kMinute, kHigh + 1'000).empty());
    EXPECT_TRUE(f.trade(kRangeEnd + 2 * kMinute, kLow - 1'000).empty());
    EXPECT_EQ(f.orb.stats().no_range_days, 1u);
    EXPECT_FALSE(f.orb.stats().has_range);
}

TEST(OrbTest, AnEntryWaitsForATouchAndATradingBookWithoutUsingItsDirection)
{
    Fixture f;
    f.range();
    f.status(TradingState::Halted);
    EXPECT_TRUE(f.trade(kRangeEnd + kMinute, kHigh + 1).empty());
    f.status(TradingState::Trading);
    ASSERT_NO_FATAL_FAILURE(expect_order(f.trade(kRangeEnd + 2 * kMinute, kHigh + 2), Side::Buy, kAsk, 100));
    EXPECT_EQ(f.orb.stats().long_entries, 1u);
}

TEST(OrbTest, APositionCarriedIntoTheNextDayIsClosedBeforeAnyEntry)
{
    Fixture f({{kOpen, kClose}, {kDay + kOpen, kDay + kClose}});
    f.go_long();
    f.status(TradingState::Halted);
    EXPECT_TRUE(f.timer(kFlatten).empty());  // halted through the close: the long is carried
    f.status(TradingState::Trading);

    const std::vector<OrderRequest> close = f.trade(kDay + kOpen + kMinute, 1'000'300);  // day 2's first range trade
    ASSERT_NO_FATAL_FAILURE(expect_order(close, Side::Sell, kBid, 100));
    f.complete(close[0], 100);
    EXPECT_EQ(f.portfolio.position(), 0);
    EXPECT_EQ(f.orb.stats().range_high, Price{1'000'300});  // day 2's range, from its own trades

    f.trade(kDay + kRangeEnd - 1, 1'000'100);
    ASSERT_NO_FATAL_FAILURE(
        expect_order(f.trade(kDay + kRangeEnd, 1'000'301), Side::Buy, kAsk, 100));  // day 2's long, its own
}

// With room for two orders, the strategy's own rule is what keeps one in flight.
TEST(OrbTest, NothingIsSentWhileAnOrderIsInFlight)
{
    Fixture f{{{kOpen, kClose}}, 2};
    f.range();
    const std::vector<OrderRequest> entry = f.trade(kRangeEnd + kMinute, kHigh + 1);
    ASSERT_EQ(entry.size(), 1u);
    EXPECT_TRUE(f.trade(kRangeEnd + kMinute + 1, kLow - 1).empty());  // the entry has not reported
    f.complete(entry[0], 100);

    ASSERT_EQ(f.trade(kRangeEnd + 2 * kMinute, kLow - 1).size(), 1u);  // the stop's IOC
    EXPECT_TRUE(f.event(kRangeEnd + 2 * kMinute + 1).empty());         // closing waits for it too
}

// A position the ORB did not open, such as manual trades before it was attached, is closed before any entry.
TEST(OrbTest, APositionItDidNotOpenIsClosedFirst)
{
    Fixture f;
    f.inherit(Side::Buy, 300);  // more than the clip: closed 100 at a time
    for (std::int64_t left = 300; left > 0; left -= 100) {
        const std::vector<OrderRequest> close = f.event(kOpen - kMinute + (300 - left));
        ASSERT_NO_FATAL_FAILURE(expect_order(close, Side::Sell, kBid, 100));
        f.complete(close[0], 100);
    }
    EXPECT_EQ(f.portfolio.position(), 0);
    EXPECT_TRUE(f.event(kOpen).empty());
    EXPECT_EQ(f.orb.stats().flattens, 1u);
    EXPECT_EQ(f.orb.stats().closing_orders, 3u);
}

// A rejected entry ends like an expired one: its direction is used, and the strategy is free again.
TEST(OrbTest, ARejectedEntryUsesItsDirectionAndFreesTheStrategy)
{
    Fixture f;
    f.range();
    const std::vector<OrderRequest> entry = f.trade(kRangeEnd + kMinute, kHigh + 1);
    ASSERT_EQ(entry.size(), 1u);
    f.report(ExecutionReport{
        .order_id = entry[0].id, .quantity = Quantity{100}, .type = ReportType::Rejected, .side = Side::Buy});
    EXPECT_TRUE(f.trade(kRangeEnd + 2 * kMinute, kHigh + 2).empty());
    ASSERT_NO_FATAL_FAILURE(expect_order(f.trade(kRangeEnd + 3 * kMinute, kLow - 1), Side::Sell, kBid, 100));
    EXPECT_EQ(f.orb.stats().expired, 100u);
    EXPECT_EQ(f.portfolio.position(), 0);
}

// The risk check refuses an entry while the one open-order slot is held: the long stays unused.
TEST(OrbTest, ARiskRejectedEntryKeepsItsDirection)
{
    Fixture f{{{kOpen, kClose}}, 1};
    f.range();
    ASSERT_EQ(f.portfolio.reserve(Side::Buy, Quantity{1}), RiskCheck::Accepted);  // the slot, held
    EXPECT_TRUE(f.trade(kRangeEnd + kMinute, kHigh + 1).empty());
    EXPECT_EQ(f.orb.stats().risk_rejects, 1u);
    f.portfolio.on_report(ExecutionReport{.quantity = Quantity{1}, .type = ReportType::Expired, .side = Side::Buy});
    ASSERT_NO_FATAL_FAILURE(expect_order(f.trade(kRangeEnd + 2 * kMinute, kHigh + 1), Side::Buy, kAsk, 100));
    EXPECT_EQ(f.orb.stats().long_entries, 1u);
}

// A day that never reaches its range end, with no trade in the range, is counted when the next day starts.
TEST(OrbTest, ADayThatNeverReachedItsRangeEndIsCountedAtTheNextDay)
{
    Fixture f({{kOpen, kClose}, {kDay + kOpen, kDay + kClose}});
    EXPECT_TRUE(f.event(kOpen + kMinute).empty());
    EXPECT_TRUE(f.event(kDay + kOpen).empty());
    EXPECT_EQ(f.orb.stats().no_range_days, 1u);
}

} // namespace
} // namespace simulator
