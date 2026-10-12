#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "simulator/market/market_book.hpp"
#include "simulator/sim/execution_simulator.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 3;
constexpr Timestamp kNow = 21'000;

MarketEvent add(OrderId id, Side side, std::int64_t price, std::uint64_t quantity)
{
    return MarketEvent{.order_id = id, .price = Price{price}, .quantity = Quantity{quantity},
                       .symbol = kSymbol, .type = MarketEventType::Add, .side = side};
}

// Spec §4.10 worked example 1's asks, plus two bid levels:
//   bids 999,900 x 300 (h4), 999,800 x 400 (h5)
//   asks 1,000,000 x 300 (h1 200, h2 100), 1,000,100 x 500 (h3)
void build_example_book(MarketBook& book)
{
    for (const MarketEvent& event : {add(1, Side::Sell, 1'000'000, 200), add(2, Side::Sell, 1'000'000, 100),
                                     add(3, Side::Sell, 1'000'100, 500), add(4, Side::Buy, 999'900, 300),
                                     add(5, Side::Buy, 999'800, 400)}) {
        ASSERT_EQ(book.apply(event), ApplyResult::Applied);
    }
}

OrderRequest order(OrderId id, Side side, std::int64_t price, std::uint64_t quantity,
                   TimeInForce time_in_force = TimeInForce::Ioc)
{
    return OrderRequest{.id = id, .side = side, .price = Price{price}, .quantity = Quantity{quantity},
                        .time_in_force = time_in_force};
}

ExecutionReport fill(OrderId id, Side side, std::int64_t price, std::uint64_t quantity, std::uint64_t leaves)
{
    return ExecutionReport{.exchange_time = kNow, .order_id = id, .price = Price{price},
                           .quantity = Quantity{quantity}, .leaves = Quantity{leaves},
                           .type = ReportType::Fill, .liquidity = Liquidity::Taker, .side = side};
}

ExecutionReport expired(OrderId id, Side side, std::uint64_t quantity)
{
    return ExecutionReport{.exchange_time = kNow, .order_id = id, .quantity = Quantity{quantity},
                           .type = ReportType::Expired, .side = side};
}

ExecutionReport rejected(OrderId id, Side side, std::uint64_t quantity, RejectReason reason)
{
    return ExecutionReport{.exchange_time = kNow, .order_id = id, .quantity = Quantity{quantity},
                           .type = ReportType::Rejected, .reason = reason, .side = side};
}

class ExecutionSimulatorTest : public ::testing::Test
{
protected:
    MarketBook book_{kSymbol, 16};
    ExecutionSimulator execution_;
    std::vector<ExecutionReport> out_;

    void SetUp() override { build_example_book(book_); }

    std::vector<ExecutionReport> arrive(const OrderRequest& request)
    {
        out_.clear();
        execution_.on_order_arrive(request, kNow, book_, out_);
        return out_;
    }
};

TEST_F(ExecutionSimulatorTest, FullFillAtOneLevel)
{
    EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'000, 150)),
              (std::vector{fill(1, Side::Buy, 1'000'000, 150, 0)}));
}

// Spec §4.10 worked example 1: sweep, partial, Expired, and no impact.
TEST_F(ExecutionSimulatorTest, WorkedExample1SweepsThenExpiresAndRepeatsIdentically)
{
    const std::uint64_t digest = book_.digest();

    EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'100, 1000)),
              (std::vector{fill(1, Side::Buy, 1'000'000, 300, 700), fill(1, Side::Buy, 1'000'100, 500, 200),
                           expired(1, Side::Buy, 200)}));
    EXPECT_EQ(book_.digest(), digest);
    EXPECT_EQ(book_.quantity_at(Side::Sell, Price{1'000'000}), 300u);
    EXPECT_EQ(book_.quantity_at(Side::Sell, Price{1'000'100}), 500u);

    // The same visible shares are taken again (spec §11 Q2).
    EXPECT_EQ(arrive(order(2, Side::Buy, 1'000'100, 1000)),
              (std::vector{fill(2, Side::Buy, 1'000'000, 300, 700), fill(2, Side::Buy, 1'000'100, 500, 200),
                           expired(2, Side::Buy, 200)}));
    EXPECT_EQ(book_.digest(), digest);
}

TEST_F(ExecutionSimulatorTest, SellSweepsBidsBestFirstAtEachLevelsPrice)
{
    const std::uint64_t digest = book_.digest();

    EXPECT_EQ(arrive(order(1, Side::Sell, 999'800, 500)),
              (std::vector{fill(1, Side::Sell, 999'900, 300, 200), fill(1, Side::Sell, 999'800, 200, 0)}));
    EXPECT_EQ(book_.digest(), digest);
}

TEST_F(ExecutionSimulatorTest, PartialFillThenExpiredWhenTheNextLevelDoesNotCross)
{
    EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'000, 400)),
              (std::vector{fill(1, Side::Buy, 1'000'000, 300, 100), expired(1, Side::Buy, 100)}));
}

TEST_F(ExecutionSimulatorTest, NoFillWhenTheLimitDoesNotCross)
{
    EXPECT_EQ(arrive(order(1, Side::Buy, 999'999, 100)), (std::vector{expired(1, Side::Buy, 100)}));
    EXPECT_EQ(arrive(order(2, Side::Sell, 999'901, 100)), (std::vector{expired(2, Side::Sell, 100)}));
}

TEST_F(ExecutionSimulatorTest, EmptyOppositeSideExpiresEverything)
{
    MarketBook bids_only{kSymbol, 4};
    ASSERT_EQ(bids_only.apply(add(1, Side::Buy, 999'900, 300)), ApplyResult::Applied);

    execution_.on_order_arrive(order(1, Side::Buy, 2'000'000, 100), kNow, bids_only, out_);
    EXPECT_EQ(out_, (std::vector{expired(1, Side::Buy, 100)}));
}

TEST_F(ExecutionSimulatorTest, RejectsDayOrdersUntilIncrement2)
{
    const std::uint64_t digest = book_.digest();

    EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'100, 100, TimeInForce::Day)),
              (std::vector{rejected(1, Side::Buy, 100, RejectReason::UnsupportedTimeInForce)}));
    EXPECT_EQ(book_.digest(), digest);
}

TEST_F(ExecutionSimulatorTest, RejectsZeroQuantity)
{
    EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'100, 0)),
              (std::vector{rejected(1, Side::Buy, 0, RejectReason::InvalidOrder)}));
}

TEST_F(ExecutionSimulatorTest, AppendsToOutAndOnMarketEmitsNothing)
{
    out_.push_back(expired(9, Side::Sell, 1));
    execution_.on_market(add(6, Side::Sell, 1'000'000, 100), book_, out_);
    execution_.on_order_arrive(order(1, Side::Buy, 1'000'000, 10), kNow, book_, out_);

    EXPECT_EQ(out_, (std::vector{expired(9, Side::Sell, 1), fill(1, Side::Buy, 1'000'000, 10, 0)}));
}

} // namespace
} // namespace simulator
