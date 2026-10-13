#include <gtest/gtest.h>

#include "simulator/orders/order_book.hpp"
#include "simulator/orders/order_pool.hpp"

namespace simulator {

namespace {

TEST(OrderPoolIntegrationTest, PooledOrdersCanBeAddedToOrderBook)
{
    OrderPool pool{10};
    OrderBook book;

    Order* order = pool.acquire(1, Side::Buy, Price{100}, Quantity{100}, Timestamp{10});

    ASSERT_NE(order, nullptr);

    ASSERT_TRUE(order->activate());
    ASSERT_TRUE(book.add(*order));

    ASSERT_NE(book.find(order->id()), nullptr);
    EXPECT_EQ(book.find(order->id()), order);
    EXPECT_EQ(book.best_bid(), order);

    EXPECT_EQ(pool.in_use(), 1);
    EXPECT_EQ(pool.available(), 9);
}

TEST(OrderPoolIntegrationTest, RejectsReleaseWhileInBook)
{
    OrderPool pool{2};
    OrderBook book;

    Order* order = pool.acquire(1, Side::Buy, Price{100}, Quantity{100}, Timestamp{10});

    ASSERT_NE(order, nullptr);
    ASSERT_TRUE(order->activate());
    ASSERT_TRUE(book.add(*order));

    // Releasing a resting order would leave a dangling pointer in the book.
    EXPECT_FALSE(pool.release(*order));

    EXPECT_EQ(pool.in_use(), 1);
    EXPECT_EQ(book.find(1), order);
}

} // namespace

} // namespace simulator