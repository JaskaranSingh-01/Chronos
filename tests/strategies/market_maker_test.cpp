#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "simulator/strategies/market_maker.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 1;
constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};
constexpr std::int64_t kBid = 999'900;
constexpr std::int64_t kAsk = 1'000'000;

static_assert(Strategy<MarketMaker>);

struct Fixture
{
    MarketBook book{kSymbol, 64};
    SimQueue queue{16};
    Portfolio portfolio;
    OrderGateway gateway{queue, portfolio, LatencyConfig{.order_latency = 10}};
    StrategyContext ctx{book, portfolio, gateway};
    MarketMaker mm;
    OrderId next_id{1};

    explicit Fixture(MarketMakerParams params = {.quote_size = 100, .max_position = 200, .requote_cooldown_ns = 1'000})
        : portfolio(kEquity,
                    RiskLimits{.max_position = 1'000, .max_order_quantity = Quantity{100}, .max_open_orders = 2},
                    FeeSchedule{}),
          mm(params)
    {
        add(Side::Buy, kBid);
        add(Side::Sell, kAsk);
    }

    void add(Side side, std::int64_t price)
    {
        const MarketEvent event{
            .order_id = next_id++, .price = Price{price}, .quantity = Quantity{100}, .symbol = kSymbol, .side = side};
        ASSERT_EQ(book.apply(event), ApplyResult::Applied);
    }

    void status(TradingState state)
    {
        ASSERT_EQ(book.apply(MarketEvent{.symbol = kSymbol, .type = MarketEventType::Status, .trading_state = state}),
                  ApplyResult::Applied);
    }

    std::vector<SimEvent> pushed()
    {
        std::vector<SimEvent> out;
        for (; !queue.empty(); queue.pop()) {
            out.push_back(queue.top());
        }
        return out;
    }

    // A market callback at `now`; returns the orders and cancels it queued.
    std::vector<SimEvent> tick(Timestamp now)
    {
        gateway.set_now(now);
        mm.on_market(ctx, MarketEvent{.timestamp = now});
        return pushed();
    }

    // Delivers `report` as the EventLoop does: the Portfolio first.
    std::vector<SimEvent> deliver(Timestamp now, ExecutionReport report)
    {
        gateway.set_now(now);
        portfolio.on_report(report);
        mm.on_report(ctx, report);
        return pushed();
    }
};

bool is_quote(const SimEvent& e, Side side, std::int64_t price)
{
    return e.type == SimEventType::OrderArrive && e.order.side == side && e.order.price == Price{price} &&
        e.order.quantity == Quantity{100} && e.order.time_in_force == TimeInForce::Day;
}

bool is_cancel(const SimEvent& e, OrderId id)
{
    return e.type == SimEventType::CancelArrive && e.order_id == id;
}

ExecutionReport fill(OrderId id, Side side, std::int64_t price, std::uint64_t quantity, std::uint64_t leaves)
{
    return ExecutionReport{.order_id = id,
                           .price = Price{price},
                           .quantity = Quantity{quantity},
                           .leaves = Quantity{leaves},
                           .type = ReportType::Fill,
                           .liquidity = Liquidity::Maker,
                           .side = side};
}

TEST(MarketMakerTest, QuotesBothSidesAtTheBestOnce)
{
    Fixture f;

    const std::vector<SimEvent> sent = f.tick(5'000);
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_TRUE(is_quote(sent[0], Side::Buy, kBid));
    EXPECT_TRUE(is_quote(sent[1], Side::Sell, kAsk));

    EXPECT_TRUE(f.tick(9'000).empty());  // live at the desired prices: nothing to do
    EXPECT_EQ(f.mm.stats().quotes, 2u);
    EXPECT_EQ(f.mm.stats().cancels, 0u);
}

TEST(MarketMakerTest, ImprovesOnlyWhileTheSpreadStaysOpen)
{
    // Spread 100 = 2 x 50: improving would lock our own quotes, so join.
    Fixture narrow{{.quote_size = 100, .max_position = 200, .requote_cooldown_ns = 1'000, .improve_ticks = 50}};
    std::vector<SimEvent> sent = narrow.tick(5'000);
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_TRUE(is_quote(sent[0], Side::Buy, kBid));
    EXPECT_TRUE(is_quote(sent[1], Side::Sell, kAsk));

    Fixture open{{.quote_size = 100, .max_position = 200, .requote_cooldown_ns = 1'000, .improve_ticks = 30}};
    sent = open.tick(5'000);
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_TRUE(is_quote(sent[0], Side::Buy, kBid + 30));
    EXPECT_TRUE(is_quote(sent[1], Side::Sell, kAsk - 30));
}

TEST(MarketMakerTest, StaysOutOfAOneSidedOrCrossedBook)
{
    Fixture f;
    f.add(Side::Buy, kAsk);  // locked
    EXPECT_TRUE(f.tick(5'000).empty());

    Fixture g;
    MarketBook bids_only{kSymbol, 4};
    ASSERT_EQ(
        bids_only.apply(MarketEvent{
            .order_id = 1, .price = Price{kBid}, .quantity = Quantity{100}, .symbol = kSymbol, .side = Side::Buy}),
        ApplyResult::Applied);
    StrategyContext ctx{bids_only, g.portfolio, g.gateway};
    g.mm.on_market(ctx, MarketEvent{});
    EXPECT_TRUE(g.pushed().empty());
}

TEST(MarketMakerTest, DropsTheSideThatWouldBreachMaxPosition)
{
    Fixture f{{.quote_size = 100, .max_position = 150, .requote_cooldown_ns = 1'000}};
    ASSERT_EQ(f.tick(5'000).size(), 2u);  // bid id 1, ask id 2

    // +60: another 100 could reach 160 > 150, so the rest of the bid is
    // cancelled at once, cooldown or not; the ask stays.
    const std::vector<SimEvent> after = f.deliver(5'100, fill(1, Side::Buy, kBid, 60, 40));
    ASSERT_EQ(after.size(), 1u);
    EXPECT_TRUE(is_cancel(after[0], 1));
    EXPECT_TRUE(
        f.deliver(
             5'200,
             ExecutionReport{.order_id = 1, .quantity = Quantity{40}, .type = ReportType::Cancelled, .side = Side::Buy})
            .empty());  // no new bid
    EXPECT_TRUE(f.tick(9'000).empty());

    // Short side: from -100 only the bid is quoted.
    Fixture g{{.quote_size = 100, .max_position = 150, .requote_cooldown_ns = 1'000}};
    ASSERT_EQ(g.portfolio.reserve(Side::Sell, Quantity{100}), RiskCheck::Accepted);
    g.portfolio.on_report(fill(99, Side::Sell, kAsk, 100, 0));
    const std::vector<SimEvent> sent = g.tick(5'000);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_TRUE(is_quote(sent[0], Side::Buy, kBid));
}

// Cancel first, send the new quote when the Cancelled report arrives.
TEST(MarketMakerTest, RequotesAfterTheBestMovesRespectingTheCooldown)
{
    Fixture f;  // cooldown 1,000
    ASSERT_EQ(f.tick(5'000).size(), 2u);

    f.add(Side::Buy, kBid + 50);
    EXPECT_TRUE(f.tick(5'999).empty());  // still cooling down

    std::vector<SimEvent> sent = f.tick(6'000);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_TRUE(is_cancel(sent[0], 1));
    EXPECT_TRUE(f.tick(6'001).empty());  // waiting for the cancel

    sent = f.deliver(
        6'100,
        ExecutionReport{.order_id = 1, .quantity = Quantity{100}, .type = ReportType::Cancelled, .side = Side::Buy});
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_TRUE(is_quote(sent[0], Side::Buy, kBid + 50));
    EXPECT_EQ(sent[0].order.id, 3u);

    // A late CancelRejected for an order already done changes nothing.
    EXPECT_TRUE(f.deliver(6'200, ExecutionReport{.order_id = 1, .type = ReportType::CancelRejected}).empty());

    const MarketMakerStats& stats = f.mm.stats();
    EXPECT_EQ(stats.quotes, 3u);
    EXPECT_EQ(stats.cancels, 1u);
}

TEST(MarketMakerTest, CountsFillsAndBacksOffAfterAWouldCrossReject)
{
    Fixture f;
    ASSERT_EQ(f.tick(5'000).size(), 2u);

    EXPECT_TRUE(f.deliver(5'100,
                          ExecutionReport{.order_id = 2,
                                          .quantity = Quantity{100},
                                          .type = ReportType::Rejected,
                                          .reason = RejectReason::WouldCross,
                                          .side = Side::Sell})
                    .empty());
    EXPECT_TRUE(f.tick(6'099).empty());  // backing off until 5,100 + 1,000
    std::vector<SimEvent> sent = f.tick(6'100);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_TRUE(is_quote(sent[0], Side::Sell, kAsk));

    // The bid fills in full; it is quoted again once its cooldown is over (it is).
    sent = f.deliver(6'200, fill(1, Side::Buy, kBid, 100, 0));
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_TRUE(is_quote(sent[0], Side::Buy, kBid));

    const MarketMakerStats& stats = f.mm.stats();
    EXPECT_EQ(stats.quotes, 4u);
    EXPECT_EQ(stats.rejects, 1u);
    EXPECT_EQ(stats.fills, 1u);
    EXPECT_EQ(stats.bought, 100u);
    EXPECT_EQ(stats.sold, 0u);
}

// A halt pulls both quotes at once, inside the cooldown; nothing is quoted
// until Trading resumes.
TEST(MarketMakerTest, PullsItsQuotesAndStaysOutWhileNotTrading)
{
    Fixture f;
    ASSERT_EQ(f.tick(5'000).size(), 2u);  // bid id 1, ask id 2

    f.status(TradingState::Halted);
    const std::vector<SimEvent> pulled = f.tick(5'100);
    ASSERT_EQ(pulled.size(), 2u);
    EXPECT_TRUE(is_cancel(pulled[0], 1));
    EXPECT_TRUE(is_cancel(pulled[1], 2));

    EXPECT_TRUE(
        f.deliver(5'200,
                  ExecutionReport{
                      .order_id = 1, .quantity = Quantity{100}, .type = ReportType::Cancelled, .side = Side::Buy})
            .empty());
    EXPECT_TRUE(
        f.deliver(5'200,
                  ExecutionReport{
                      .order_id = 2, .quantity = Quantity{100}, .type = ReportType::Cancelled, .side = Side::Sell})
            .empty());
    EXPECT_TRUE(f.tick(9'000).empty());

    f.status(TradingState::Trading);
    const std::vector<SimEvent> sent = f.tick(9'100);
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_TRUE(is_quote(sent[0], Side::Buy, kBid));
    EXPECT_TRUE(is_quote(sent[1], Side::Sell, kAsk));
}

} // namespace
} // namespace simulator
