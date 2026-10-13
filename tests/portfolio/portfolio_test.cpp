#include <gtest/gtest.h>

#include <cstdint>

#include "simulator/market/market_book.hpp"
#include "simulator/portfolio/portfolio.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 1;

// US equity: prices in $0.0001 ticks, whole shares, so money is in tick-shares.
constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};

constexpr RiskLimits kWide{.max_position = 1'000'000, .max_order_quantity = Quantity{1'000'000}, .max_open_orders = 16};

ExecutionReport fill(Side side, std::int64_t price, std::uint64_t quantity, std::uint64_t leaves,
                     Liquidity liquidity = Liquidity::Taker)
{
    return ExecutionReport{.price = Price{price},
                           .quantity = Quantity{quantity},
                           .leaves = Quantity{leaves},
                           .type = ReportType::Fill,
                           .liquidity = liquidity,
                           .side = side};
}

// Cancelled, Expired or Rejected: `quantity` leaves the market.
ExecutionReport closed(ReportType type, Side side, std::uint64_t quantity)
{
    return ExecutionReport{.quantity = Quantity{quantity}, .type = type, .side = side};
}

// Reserve, then fill all of it in one report.
void trade(Portfolio& portfolio, Side side, std::int64_t price, std::uint64_t quantity,
           Liquidity liquidity = Liquidity::Taker)
{
    ASSERT_EQ(portfolio.reserve(side, Quantity{quantity}), RiskCheck::Accepted);
    portfolio.on_report(fill(side, price, quantity, 0, liquidity));
}

// Marks against a book quoting `bid` x `ask`; 0 leaves that side empty.
void mark_at(Portfolio& portfolio, std::int64_t bid, std::int64_t ask)
{
    MarketBook book{kSymbol, 4};
    if (bid != 0) {
        ASSERT_EQ(
            book.apply(MarketEvent{
                .order_id = 1, .price = Price{bid}, .quantity = Quantity{100}, .symbol = kSymbol, .side = Side::Buy}),
            ApplyResult::Applied);
    }
    if (ask != 0) {
        ASSERT_EQ(
            book.apply(MarketEvent{
                .order_id = 2, .price = Price{ask}, .quantity = Quantity{100}, .symbol = kSymbol, .side = Side::Sell}),
            ApplyResult::Applied);
    }
    portfolio.mark(book);
}

TEST(PortfolioTest, WorkedExample1CashAndFees)
{
    Portfolio portfolio{kEquity, kWide, FeeSchedule{.taker_fee_per_share = 30}};
    ASSERT_EQ(portfolio.reserve(Side::Buy, Quantity{1000}), RiskCheck::Accepted);

    portfolio.on_report(fill(Side::Buy, 1'000'000, 300, 700));
    portfolio.on_report(fill(Side::Buy, 1'000'100, 500, 200));
    portfolio.on_report(closed(ReportType::Expired, Side::Buy, 200));

    EXPECT_EQ(portfolio.position(), 800);
    EXPECT_EQ(portfolio.cash(), -800'050'000);  // -$80,005.00
    EXPECT_EQ(portfolio.fees(), 24'000);        // $2.40
    EXPECT_EQ(portfolio.realised_pnl(), 0);
    EXPECT_EQ(portfolio.open_orders(), 0u);
    EXPECT_EQ(portfolio.open_quantity(Side::Buy), 0u);

    // Long, so marked at the best bid: 800 x 1,000,000 - 800,050,000.
    mark_at(portfolio, 1'000'000, 1'000'100);
    EXPECT_EQ(portfolio.unrealised_pnl(), -50'000);
    EXPECT_EQ(portfolio.pnl(), -74'000);
}

TEST(PortfolioTest, BuyThenSellRealisesPnlNetOfMakerAndTakerFees)
{
    Portfolio portfolio{kEquity, kWide, FeeSchedule{.maker_fee_per_share = -20, .taker_fee_per_share = 30}};

    trade(portfolio, Side::Buy, 1'000'000, 100, Liquidity::Taker);
    trade(portfolio, Side::Sell, 1'000'500, 100, Liquidity::Maker);  // rebate

    EXPECT_EQ(portfolio.position(), 0);
    EXPECT_EQ(portfolio.cash(), 50'000);
    EXPECT_EQ(portfolio.fees(), 3'000 - 2'000);
    EXPECT_EQ(portfolio.realised_pnl(), 50'000);
    EXPECT_EQ(portfolio.unrealised_pnl(), 0);
    EXPECT_EQ(portfolio.pnl(), 49'000);
    EXPECT_EQ(portfolio.max_abs_position(), 100);
}

TEST(PortfolioTest, ShortPositionRealisesPnlOnBuyBack)
{
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};

    trade(portfolio, Side::Sell, 1'000'000, 100);
    EXPECT_EQ(portfolio.position(), -100);
    EXPECT_EQ(portfolio.cash(), 100'000'000);

    // Short, so marked at the best ask.
    mark_at(portfolio, 999'000, 999'500);
    EXPECT_EQ(portfolio.unrealised_pnl(), 50'000);

    trade(portfolio, Side::Buy, 999'000, 100);
    EXPECT_EQ(portfolio.position(), 0);
    EXPECT_EQ(portfolio.realised_pnl(), 100'000);
    EXPECT_EQ(portfolio.unrealised_pnl(), 0);
    EXPECT_EQ(portfolio.pnl(), 100'000);
    EXPECT_EQ(portfolio.max_abs_position(), 100);
}

TEST(PortfolioTest, MarksLongAtBidShortAtAskAndKeepsTheLastMarkOfAnEmptySide)
{
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
    trade(portfolio, Side::Buy, 1'000'000, 100);

    mark_at(portfolio, 999'900, 1'000'200);
    EXPECT_EQ(portfolio.unrealised_pnl(), -10'000);

    mark_at(portfolio, 0, 1'000'300);  // no bid: keep 999,900
    EXPECT_EQ(portfolio.unrealised_pnl(), -10'000);

    // Flip to short 200: the closed 100 realise 0, the new short opens at
    // 1,000,000 and uses the last ask seen (1,000,300) until the next mark.
    trade(portfolio, Side::Sell, 1'000'000, 300);
    EXPECT_EQ(portfolio.position(), -200);
    EXPECT_EQ(portfolio.realised_pnl(), 0);
    EXPECT_EQ(portfolio.unrealised_pnl(), -60'000);

    mark_at(portfolio, 999'800, 1'000'100);
    EXPECT_EQ(portfolio.unrealised_pnl(), -20'000);
    EXPECT_EQ(portfolio.max_abs_position(), 200);
}

TEST(PortfolioTest, AveragePriceAcrossPartialFills)
{
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
    ASSERT_EQ(portfolio.reserve(Side::Buy, Quantity{400}), RiskCheck::Accepted);
    portfolio.on_report(fill(Side::Buy, 1'000'000, 100, 300));
    portfolio.on_report(fill(Side::Buy, 1'000'400, 300, 0));  // average 1,000,300

    trade(portfolio, Side::Sell, 1'000'500, 200);
    EXPECT_EQ(portfolio.realised_pnl(), 200 * 200);

    mark_at(portfolio, 1'000'300, 1'000'400);  // the remaining 200 at their average
    EXPECT_EQ(portfolio.unrealised_pnl(), 0);
}

TEST(PortfolioTest, FractionalAverageCostRoundsWithoutLeakingAcrossTheRoundTrip)
{
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
    trade(portfolio, Side::Buy, 100, 1);
    trade(portfolio, Side::Buy, 101, 2);  // average 100.67 ticks

    trade(portfolio, Side::Sell, 102, 1);
    EXPECT_EQ(portfolio.realised_pnl(), 2);  // cost 302 / 3 truncates to 100

    trade(portfolio, Side::Sell, 102, 2);    // flat: realised is exactly the cash
    EXPECT_EQ(portfolio.realised_pnl(), 3 * 102 - 302);
    EXPECT_EQ(portfolio.realised_pnl(), portfolio.cash());
}

TEST(PortfolioTest, MaxDrawdownIsTheLargestFallFromAPeak)
{
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
    trade(portfolio, Side::Buy, 1'000'000, 100);
    EXPECT_EQ(portfolio.max_drawdown(), 0);

    mark_at(portfolio, 1'000'500, 0);  // pnl  +50,000, peak 50,000
    EXPECT_EQ(portfolio.max_drawdown(), 0);
    mark_at(portfolio, 999'000, 0);    // pnl -100,000
    EXPECT_EQ(portfolio.max_drawdown(), 150'000);
    mark_at(portfolio, 1'001'000, 0);  // pnl +100,000, peak 100,000
    mark_at(portfolio, 1'000'000, 0);  // pnl 0: a 100,000 fall, smaller
    EXPECT_EQ(portfolio.max_drawdown(), 150'000);
    mark_at(portfolio, 998'000, 0);    // pnl -200,000
    EXPECT_EQ(portfolio.max_drawdown(), 300'000);
    EXPECT_EQ(portfolio.pnl(), -200'000);
}

// BTC-like: prices in cents, sizes in 1e-8 units, so money is in 1e-10 USD.
TEST(PortfolioTest, QuantityScale8Instrument)
{
    Portfolio portfolio{
        Instrument{.price_scale = 2, .quantity_scale = 8},
        RiskLimits{.max_position = 100'000'000, .max_order_quantity = Quantity{100'000'000}, .max_open_orders = 4},
        FeeSchedule{}};

    trade(portfolio, Side::Buy, 6'000'000, 50'000'000);  // 0.5 @ $60,000.00
    EXPECT_EQ(portfolio.position(), 50'000'000);
    EXPECT_EQ(portfolio.cash(), -300'000'000'000'000);  // -$30,000

    mark_at(portfolio, 6'010'000, 0);                    // bid $60,100.00
    EXPECT_EQ(portfolio.unrealised_pnl(), 500'000'000'000);  // $50
}

// At the combined-scale cap (4 + 8 = 12) a 100-unit position at $90,000 is
// 9e18 units, just under INT64_MAX; closing half needs cost x 5e9 = 4.5e28.
TEST(PortfolioTest, ProductsNearTheScaleCapStayExact)
{
    Portfolio portfolio{Instrument{.price_scale = 4, .quantity_scale = 8},
                        RiskLimits{.max_position = 20'000'000'000,
                                   .max_order_quantity = Quantity{20'000'000'000},
                                   .max_open_orders = 4},
                        FeeSchedule{}};

    trade(portfolio, Side::Buy, 900'000'000, 10'000'000'000);
    EXPECT_EQ(portfolio.cash(), -9'000'000'000'000'000'000);

    trade(portfolio, Side::Sell, 900'000'100, 5'000'000'000);
    EXPECT_EQ(portfolio.realised_pnl(), 500'000'000'000);
    EXPECT_EQ(portfolio.cash(), -4'499'999'500'000'000'000);

    mark_at(portfolio, 900'000'200, 0);
    EXPECT_EQ(portfolio.unrealised_pnl(), 1'000'000'000'000);
    EXPECT_EQ(portfolio.pnl(), 1'500'000'000'000);
}

TEST(PortfolioTest, ReserveReturnsEachRiskCheck)
{
    Portfolio portfolio{kEquity,
                        RiskLimits{.max_position = 100, .max_order_quantity = Quantity{60}, .max_open_orders = 2},
                        FeeSchedule{}};

    EXPECT_EQ(portfolio.reserve(Side::Buy, Quantity{0}), RiskCheck::InvalidOrder);
    EXPECT_EQ(portfolio.reserve(Side::Buy, Quantity{61}), RiskCheck::OrderTooLarge);
    EXPECT_EQ(portfolio.reserve(Side::Buy, Quantity{60}), RiskCheck::Accepted);
    EXPECT_EQ(portfolio.reserve(Side::Buy, Quantity{41}), RiskCheck::PositionLimit);  // 60 open + 41
    EXPECT_EQ(portfolio.reserve(Side::Buy, Quantity{40}), RiskCheck::Accepted);
    EXPECT_EQ(portfolio.reserve(Side::Sell, Quantity{1}), RiskCheck::TooManyOpenOrders);

    // Rejections reserve nothing.
    EXPECT_EQ(portfolio.open_orders(), 2u);
    EXPECT_EQ(portfolio.open_quantity(Side::Buy), 100u);
    EXPECT_EQ(portfolio.open_quantity(Side::Sell), 0u);
}

TEST(PortfolioTest, PositionLimitCountsPositionAndSameSideOpenQuantity)
{
    Portfolio portfolio{kEquity,
                        RiskLimits{.max_position = 100, .max_order_quantity = Quantity{200}, .max_open_orders = 8},
                        FeeSchedule{}};
    trade(portfolio, Side::Buy, 1'000'000, 70);

    EXPECT_EQ(portfolio.reserve(Side::Buy, Quantity{31}), RiskCheck::PositionLimit);
    EXPECT_EQ(portfolio.reserve(Side::Buy, Quantity{30}), RiskCheck::Accepted);

    // Worst case for sells ignores open buys: 70 - 100 - 71 = -101.
    EXPECT_EQ(portfolio.reserve(Side::Sell, Quantity{100}), RiskCheck::Accepted);
    EXPECT_EQ(portfolio.reserve(Side::Sell, Quantity{71}), RiskCheck::PositionLimit);
    EXPECT_EQ(portfolio.reserve(Side::Sell, Quantity{70}), RiskCheck::Accepted);
}

TEST(PortfolioTest, TerminalReportsReleaseTheReservation)
{
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};

    ASSERT_EQ(portfolio.reserve(Side::Buy, Quantity{100}), RiskCheck::Accepted);
    EXPECT_EQ(portfolio.open_orders(), 1u);
    EXPECT_EQ(portfolio.open_quantity(Side::Buy), 100u);

    portfolio.on_report(fill(Side::Buy, 1'000'000, 30, 70));
    EXPECT_EQ(portfolio.open_orders(), 1u);
    EXPECT_EQ(portfolio.open_quantity(Side::Buy), 70u);

    portfolio.on_report(fill(Side::Buy, 1'000'000, 50, 20));
    EXPECT_EQ(portfolio.open_quantity(Side::Buy), 20u);

    portfolio.on_report(closed(ReportType::Expired, Side::Buy, 20));
    EXPECT_EQ(portfolio.open_orders(), 0u);
    EXPECT_EQ(portfolio.open_quantity(Side::Buy), 0u);
    EXPECT_EQ(portfolio.position(), 80);

    ASSERT_EQ(portfolio.reserve(Side::Sell, Quantity{50}), RiskCheck::Accepted);
    portfolio.on_report(closed(ReportType::Cancelled, Side::Sell, 50));
    ASSERT_EQ(portfolio.reserve(Side::Sell, Quantity{50}), RiskCheck::Accepted);
    portfolio.on_report(closed(ReportType::Rejected, Side::Sell, 50));
    EXPECT_EQ(portfolio.open_orders(), 0u);
    EXPECT_EQ(portfolio.open_quantity(Side::Sell), 0u);

    trade(portfolio, Side::Sell, 1'000'000, 10);  // a full fill closes the order
    EXPECT_EQ(portfolio.open_orders(), 0u);
    EXPECT_EQ(portfolio.open_quantity(Side::Sell), 0u);
    EXPECT_EQ(portfolio.position(), 70);
}

} // namespace
} // namespace simulator
