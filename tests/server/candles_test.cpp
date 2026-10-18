#include <gtest/gtest.h>

#include <vector>

#include "chronos/server/candles.hpp"
#include "simulator/engine/event_loop.hpp"
#include "simulator/market/vector_source.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

TEST(CandlesTest, MarketTradeReadsTheExecutedOrdersPriceAndSide)
{
    MarketBook book{kSymbol, 16};
    ASSERT_EQ(book.apply(add(1, 7, Side::Buy, 999'900, 100)), ApplyResult::Applied);

    Trade trade;
    ASSERT_TRUE(market_trade(execute(2, 7, 30), book, trade));  // ITCH E: the resting order's price
    EXPECT_EQ(trade, (Trade{.timestamp = 2, .price = Price{999'900}, .quantity = 30, .side = Side::Buy}));

    ASSERT_TRUE(market_trade(execute(3, 7, 30, 999'950), book, trade));  // ITCH C: its own price
    EXPECT_EQ(trade.price, Price{999'950});

    const MarketEvent print{.timestamp = 4,
                            .price = Price{1'000'100},
                            .quantity = Quantity{5},
                            .symbol = kSymbol,
                            .type = MarketEventType::Trade,
                            .side = Side::Sell};
    ASSERT_TRUE(market_trade(print, book, trade));  // ITCH P
    EXPECT_EQ(trade, (Trade{.timestamp = 4, .price = Price{1'000'100}, .quantity = 5, .side = Side::Sell}));
}

TEST(CandlesTest, MarketTradeSkipsWhatTheBookWouldReject)
{
    MarketBook book{kSymbol, 16};
    ASSERT_EQ(book.apply(add(1, 7, Side::Buy, 999'900, 100)), ApplyResult::Applied);
    Trade trade;
    EXPECT_FALSE(market_trade(execute(2, 8, 10), book, trade));   // unknown order
    EXPECT_FALSE(market_trade(execute(2, 7, 101), book, trade));  // more than it has left
    EXPECT_FALSE(market_trade(execute(2, 7, 0), book, trade));
    EXPECT_FALSE(market_trade(add(2, 9, Side::Sell, 1'000'000, 10), book, trade));
    MarketEvent other = execute(2, 7, 10);
    other.symbol = 1;
    EXPECT_FALSE(market_trade(other, book, trade));
}

TEST(CandlesTest, APendingExecuteIsPricedFromTheBookBeforeStep)
{
    const std::vector<MarketEvent> events{add(10, 1, Side::Sell, 1'000'000, 100), execute(20, 1, 100)};
    VectorSource source{events};
    MarketBook book{kSymbol, 16};
    ExecutionSimulator execution;
    Portfolio portfolio{Instrument{.price_scale = 4, .quantity_scale = 0}, RiskLimits{}, FeeSchedule{}};
    NullStrategy strategy;
    EventLoop<NullStrategy> loop{source, book, execution, portfolio, strategy, LatencyConfig{}};
    Timestamp ts{0};
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);

    Trade trade;
    ASSERT_TRUE(market_trade(*loop.pending(), loop.book(), trade));
    EXPECT_EQ(trade, (Trade{.timestamp = 20, .price = Price{1'000'000}, .quantity = 100, .side = Side::Sell}));
    EXPECT_EQ(loop.pending()->price, Price{});  // read, never written: the fill guard reads it

    ASSERT_TRUE(loop.step().ok);
    EXPECT_FALSE(market_trade(execute(20, 1, 100), loop.book(), trade));  // after step the order is gone
}

TEST(CandlesTest, OneBarPerSecondFromTradesWithGapsForQuietSeconds)
{
    CandleBuilder candles;
    Candle bar;
    EXPECT_FALSE(candles.current(bar));

    const auto trade = [](Timestamp ts, std::int64_t price, std::uint64_t quantity) {
        return Trade{.timestamp = ts, .price = Price{price}, .quantity = quantity, .side = Side::Buy};
    };
    candles.add(trade(kSecond + 100, 10, 5));
    candles.add(trade(kSecond + 500, 12, 1));
    candles.add(trade(2 * kSecond - 1, 9, 2));
    candles.add(trade(3 * kSecond + 200, 11, 4));  // second 2 had no trade: no bar

    ASSERT_EQ(candles.closed().size(), 1u);
    EXPECT_EQ(
        candles.closed()[0],
        (Candle{
            .start = kSecond, .open = Price{10}, .high = Price{12}, .low = Price{9}, .close = Price{9}, .volume = 8}));
    ASSERT_TRUE(candles.current(bar));
    EXPECT_EQ(bar,
              (Candle{.start = 3 * kSecond,
                      .open = Price{11},
                      .high = Price{11},
                      .low = Price{11},
                      .close = Price{11},
                      .volume = 4}));

    candles.add(trade(4 * kSecond, 13, 3));  // exactly on the boundary: opens second 4, closes second 3
    ASSERT_EQ(candles.closed().size(), 2u);
    EXPECT_EQ(candles.closed()[1], bar);  // second 3's bar, as it stood
    ASSERT_TRUE(candles.current(bar));
    EXPECT_EQ(bar,
              (Candle{.start = 4 * kSecond,
                      .open = Price{13},
                      .high = Price{13},
                      .low = Price{13},
                      .close = Price{13},
                      .volume = 3}));
}

}  // namespace
}  // namespace simulator::server::test
