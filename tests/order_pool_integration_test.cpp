#include <gtest/gtest.h>

#include <vector>

#include "simulator/matching/matching_engine.hpp"
#include "simulator/orders/order_book.hpp"
#include "simulator/orders/order_pool.hpp"

namespace simulator {

namespace {

TEST(OrderPoolIntegrationTest, PooledOrdersCanBeAddedToOrderBook)
{
    OrderPool pool{10};
    OrderBook book;

    Order* order = pool.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(order, nullptr);

    ASSERT_TRUE(order->activate());
    ASSERT_TRUE(book.add(*order));

    ASSERT_NE(book.find(order->id()), nullptr);
    EXPECT_EQ(book.find(order->id()), order);
    EXPECT_EQ(book.best_bid(), order);

    EXPECT_EQ(pool.in_use(), 1);
    EXPECT_EQ(pool.available(), 9);
}

TEST(OrderPoolIntegrationTest, PooledOrdersCanBeMatched)
{
    OrderPool pool{10};
    OrderBook book;
    MatchingEngine engine{book};

    Order* sell = pool.acquire(
        1,
        Side::Sell,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(sell, nullptr);

    ASSERT_TRUE(sell->activate());
    ASSERT_TRUE(book.add(*sell));

    Order* buy = pool.acquire(
        2,
        Side::Buy,
        Price{101},
        Quantity{100},
        Timestamp{20}
    );

    ASSERT_NE(buy, nullptr);

    ASSERT_TRUE(buy->activate());

    // Reusable trade output buffer.
    std::vector<Trade> trades;
    trades.reserve(10);
    std::vector<Order*> filled_orders;
    filled_orders.reserve(10);

    const std::size_t trade_count =
        engine.submit(*buy, trades, filled_orders);

    ASSERT_EQ(trade_count, 1);
    ASSERT_EQ(trades.size(), 1);

    EXPECT_EQ(trades[0].buy_order_id, buy->id());
    EXPECT_EQ(trades[0].sell_order_id, sell->id());
    EXPECT_EQ(trades[0].price, Price{100});
    EXPECT_EQ(trades[0].quantity, Quantity{100});

    EXPECT_EQ(buy->state(), OrderState::Filled);
    EXPECT_EQ(sell->state(), OrderState::Filled);

    EXPECT_EQ(book.find(sell->id()), nullptr);
    EXPECT_EQ(book.find(buy->id()), nullptr);

    EXPECT_EQ(pool.in_use(), 2);
}

TEST(OrderPoolIntegrationTest, FilledOrderCanBeRemovedAndReleased)
{
    OrderPool pool{2};
    OrderBook book;

    Order* order = pool.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(order, nullptr);

    ASSERT_TRUE(order->activate());
    ASSERT_TRUE(book.add(*order));

    ASSERT_TRUE(
        order->execute(Quantity{100})
    );

    EXPECT_EQ(
        order->state(),
        OrderState::Filled
    );

    ASSERT_TRUE(book.remove(order->id()));

    EXPECT_EQ(book.find(order->id()), nullptr);

    ASSERT_TRUE(pool.release(*order));

    EXPECT_EQ(pool.in_use(), 0);
    EXPECT_EQ(pool.available(), 2);
}

} // namespace

} // namespace simulator