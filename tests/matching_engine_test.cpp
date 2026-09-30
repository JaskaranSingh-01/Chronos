#include <gtest/gtest.h>

#include "simulator/matching/matching_engine.hpp"

namespace simulator {
namespace {

Order make_buy_order(
    OrderId id,
    Price price,
    Quantity quantity = Quantity{100},
    Timestamp timestamp = 1)
{
    return Order{
        id,
        Side::Buy,
        price,
        quantity,
        timestamp
    };
}

Order make_sell_order(
    OrderId id,
    Price price,
    Quantity quantity = Quantity{100},
    Timestamp timestamp = 1)
{
    return Order{
        id,
        Side::Sell,
        price,
        quantity,
        timestamp
    };
}

void activate(Order& order)
{
    ASSERT_TRUE(order.activate());
}

} // namespace


TEST(MatchingEngineTest, ExactBuySellMatch)
{
    OrderBook book;
    MatchingEngine engine{book};

    // Resting sell order.
    auto sell = make_sell_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    activate(sell);

    ASSERT_TRUE(book.add(sell));

    ASSERT_NE(book.best_ask(), nullptr);
    ASSERT_EQ(book.best_ask()->id(), sell.id());
    ASSERT_EQ(book.best_ask()->price(), Price{100});
    ASSERT_EQ(book.best_ask()->remaining_quantity(), Quantity{100});

    // Incoming buy order.
    auto buy = make_buy_order(
        2,
        Price{101},
        Quantity{100},
        20
    );

    activate(buy);

    auto trades = engine.submit(buy);

    ASSERT_EQ(trades.size(), 1);

    const Trade& trade = trades[0];

    EXPECT_EQ(trade.buy_order_id, buy.id());
    EXPECT_EQ(trade.sell_order_id, sell.id());
    EXPECT_EQ(trade.price, Price{100});
    EXPECT_EQ(trade.quantity, Quantity{100});
    EXPECT_EQ(trade.timestamp, Timestamp{20});

    EXPECT_EQ(buy.state(), OrderState::Filled);
    EXPECT_EQ(sell.state(), OrderState::Filled);

    EXPECT_EQ(
        buy.remaining_quantity(),
        Quantity{0}
    );

    EXPECT_EQ(
        sell.remaining_quantity(),
        Quantity{0}
    );

    EXPECT_EQ(book.find(sell.id()), nullptr);
    EXPECT_EQ(book.find(buy.id()), nullptr);

    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask(), nullptr);

    EXPECT_TRUE(book.empty());
}

} // namespace simulator