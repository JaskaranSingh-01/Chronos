#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "simulator/strategies/imbalance_taker.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 1;
constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};
constexpr std::int64_t kBid = 999'900;
constexpr std::int64_t kAsk = 1'000'000;

static_assert(Strategy<ImbalanceTaker>);

struct Fixture
{
    MarketBook book{kSymbol, 64};
    SimQueue queue{16};
    Portfolio portfolio;
    OrderGateway gateway{queue, portfolio, LatencyConfig{.order_latency = 10}};
    StrategyContext ctx{book, portfolio, gateway};
    ImbalanceTaker taker;
    OrderId next_id{1};

    explicit Fixture(
        ImbalanceTakerParams params = {.threshold = 3.0, .clip = 100, .max_position = 200, .cooldown_ns = 1'000},
        std::size_t max_open_orders = 8)
        : portfolio(kEquity,
                    RiskLimits{
                        .max_position = 1'000, .max_order_quantity = Quantity{100}, .max_open_orders = max_open_orders},
                    FeeSchedule{}),
          taker(params)
    {}

    // Adds one order of `quantity` at our fixed bid or ask price.
    void add(Side side, std::uint64_t quantity)
    {
        const MarketEvent event{.order_id = next_id++,
                                .price = Price{side == Side::Buy ? kBid : kAsk},
                                .quantity = Quantity{quantity},
                                .symbol = kSymbol,
                                .side = side};
        ASSERT_EQ(book.apply(event), ApplyResult::Applied);
    }

    void status(TradingState state)
    {
        ASSERT_EQ(book.apply(MarketEvent{.symbol = kSymbol, .type = MarketEventType::Status, .trading_state = state}),
                  ApplyResult::Applied);
    }

    // A market callback at `now`; returns the orders it sent.
    std::vector<OrderRequest> tick(Timestamp now)
    {
        gateway.set_now(now);
        taker.on_market(ctx, MarketEvent{.timestamp = now});
        std::vector<OrderRequest> sent;
        for (; !queue.empty(); queue.pop()) {
            sent.push_back(queue.top().order);
        }
        return sent;
    }

    // Reserves and fills `quantity` at kAsk (buy) or kBid (sell).
    void fill(Side side, std::uint64_t quantity)
    {
        ASSERT_EQ(portfolio.reserve(side, Quantity{quantity}), RiskCheck::Accepted);
        portfolio.on_report(ExecutionReport{.price = Price{side == Side::Buy ? kAsk : kBid},
                                            .quantity = Quantity{quantity},
                                            .type = ReportType::Fill,
                                            .liquidity = Liquidity::Taker,
                                            .side = side});
    }
};

TEST(ImbalanceTakerTest, BuysAtTheAskWhenTheBidOutweighsTheAsk)
{
    Fixture f;
    f.add(Side::Buy, 300);
    f.add(Side::Sell, 100);

    const std::vector<OrderRequest> sent = f.tick(5'000);

    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].side, Side::Buy);
    EXPECT_EQ(sent[0].price, Price{kAsk});
    EXPECT_EQ(sent[0].quantity, Quantity{100});
    EXPECT_EQ(sent[0].time_in_force, TimeInForce::Ioc);
    EXPECT_EQ(f.taker.stats().sent, 1u);
}

TEST(ImbalanceTakerTest, SellsAtTheBidWhenTheAskOutweighsTheBid)
{
    Fixture f;
    f.add(Side::Buy, 100);
    f.add(Side::Sell, 200);
    f.add(Side::Sell, 100);  // the level, not one order, counts

    const std::vector<OrderRequest> sent = f.tick(5'000);

    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].side, Side::Sell);
    EXPECT_EQ(sent[0].price, Price{kBid});
}

TEST(ImbalanceTakerTest, StaysOutBelowTheThresholdOrWithAnEmptySide)
{
    Fixture f;
    f.add(Side::Buy, 299);
    EXPECT_TRUE(f.tick(5'000).empty());  // no ask

    f.add(Side::Sell, 100);
    EXPECT_TRUE(f.tick(5'001).empty());  // 299 < 3 x 100
    EXPECT_EQ(f.taker.stats().sent, 0u);
}

TEST(ImbalanceTakerTest, RespectsTheCooldown)
{
    Fixture f;
    f.add(Side::Buy, 300);
    f.add(Side::Sell, 100);

    EXPECT_EQ(f.tick(5'000).size(), 1u);
    EXPECT_TRUE(f.tick(5'999).empty());  // cooldown_ns = 1,000
    EXPECT_EQ(f.tick(6'000).size(), 1u);
    EXPECT_EQ(f.taker.stats().sent, 2u);
}

TEST(ImbalanceTakerTest, RespectsMaxPositionBothWays)
{
    Fixture f;
    f.add(Side::Buy, 300);
    f.add(Side::Sell, 100);
    f.fill(Side::Buy, 100);
    f.fill(Side::Buy, 100);  // +200 = max_position

    EXPECT_TRUE(f.tick(5'000).empty());  // would go to +300

    f.add(Side::Sell, 1'200);  // now ask-heavy: 300 vs 1,300
    const std::vector<OrderRequest> sent = f.tick(5'001);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].side, Side::Sell);  // reducing is fine

    Fixture g;
    g.add(Side::Buy, 100);
    g.add(Side::Sell, 300);
    g.fill(Side::Sell, 100);
    g.fill(Side::Sell, 100);  // -200
    EXPECT_TRUE(g.tick(5'000).empty());
}

TEST(ImbalanceTakerTest, CountsRiskRejectsFillsAndExpiries)
{
    Fixture f{{.threshold = 3.0, .clip = 100, .max_position = 200, .cooldown_ns = 0}, 1};
    f.add(Side::Buy, 300);
    f.add(Side::Sell, 100);

    EXPECT_EQ(f.tick(5'000).size(), 1u);
    EXPECT_TRUE(f.tick(5'001).empty());  // one order already open: TooManyOpenOrders

    f.taker.on_report(f.ctx,
                      ExecutionReport{.price = Price{kAsk},
                                      .quantity = Quantity{60},
                                      .leaves = Quantity{40},
                                      .type = ReportType::Fill,
                                      .liquidity = Liquidity::Taker,
                                      .side = Side::Buy});
    f.taker.on_report(f.ctx, ExecutionReport{.quantity = Quantity{40}, .type = ReportType::Expired, .side = Side::Buy});

    const ImbalanceTakerStats& stats = f.taker.stats();
    EXPECT_EQ(stats.sent, 1u);
    EXPECT_EQ(stats.risk_rejects, 1u);
    EXPECT_EQ(stats.fills, 1u);
    EXPECT_EQ(stats.expired, 1u);
    EXPECT_EQ(stats.bought, 60u);
    EXPECT_EQ(stats.sold, 0u);
}

TEST(ImbalanceTakerTest, StaysOutWhileNotTrading)
{
    Fixture f;
    f.add(Side::Buy, 300);
    f.add(Side::Sell, 100);

    f.status(TradingState::Paused);
    EXPECT_TRUE(f.tick(5'000).empty());
    EXPECT_EQ(f.taker.stats().sent, 0u);

    f.status(TradingState::Trading);
    const std::vector<OrderRequest> sent = f.tick(5'001);  // no cooldown started while paused
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].side, Side::Buy);
}

} // namespace
} // namespace simulator
