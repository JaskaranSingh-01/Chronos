#include <gtest/gtest.h>

#include "simulator/orders/order_pool.hpp"

namespace simulator {
namespace {

// ------------------------------------------------------------
// Basic construction
// ------------------------------------------------------------

TEST(OrderPoolTest, StartsWithFullCapacity)
{
    OrderPool pool{10};

    EXPECT_EQ(pool.capacity(), 10);
    EXPECT_EQ(pool.available(), 10);
    EXPECT_EQ(pool.in_use(), 0);
}


// ------------------------------------------------------------
// Acquire
// ------------------------------------------------------------

TEST(OrderPoolTest, AcquireReturnsOrder)
{
    OrderPool pool{10};

    Order* order = pool.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{50},
        Timestamp{10}
    );

    ASSERT_NE(order, nullptr);

    EXPECT_EQ(order->id(), 1);
    EXPECT_EQ(order->side(), Side::Buy);
    EXPECT_EQ(order->price(), Price{100});
    EXPECT_EQ(order->quantity(), Quantity{50});
    EXPECT_EQ(order->remaining_quantity(), Quantity{50});
    EXPECT_EQ(order->timestamp(), Timestamp{10});
    EXPECT_EQ(order->state(), OrderState::New);

    EXPECT_EQ(pool.available(), 9);
    EXPECT_EQ(pool.in_use(), 1);
}


// ------------------------------------------------------------
// Acquired orders are distinct
// ------------------------------------------------------------

TEST(OrderPoolTest, AcquireReturnsDistinctOrders)
{
    OrderPool pool{10};

    Order* order1 = pool.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{50},
        Timestamp{10}
    );

    Order* order2 = pool.acquire(
        2,
        Side::Sell,
        Price{101},
        Quantity{75},
        Timestamp{20}
    );

    ASSERT_NE(order1, nullptr);
    ASSERT_NE(order2, nullptr);

    EXPECT_NE(order1, order2);

    EXPECT_EQ(order1->id(), 1);
    EXPECT_EQ(order2->id(), 2);

    EXPECT_EQ(pool.available(), 8);
    EXPECT_EQ(pool.in_use(), 2);
}


// ------------------------------------------------------------
// Pool exhaustion
// ------------------------------------------------------------

TEST(OrderPoolTest, ReturnsNullWhenPoolIsExhausted)
{
    OrderPool pool{2};

    Order* order1 = pool.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{50},
        Timestamp{10}
    );

    Order* order2 = pool.acquire(
        2,
        Side::Sell,
        Price{101},
        Quantity{50},
        Timestamp{20}
    );

    ASSERT_NE(order1, nullptr);
    ASSERT_NE(order2, nullptr);

    EXPECT_EQ(pool.available(), 0);
    EXPECT_EQ(pool.in_use(), 2);

    Order* order3 = pool.acquire(
        3,
        Side::Buy,
        Price{102},
        Quantity{50},
        Timestamp{30}
    );

    EXPECT_EQ(order3, nullptr);

    EXPECT_EQ(pool.available(), 0);
    EXPECT_EQ(pool.in_use(), 2);
}


// ------------------------------------------------------------
// Release
// ------------------------------------------------------------

TEST(OrderPoolTest, ReleaseMakesSlotAvailable)
{
    OrderPool pool{2};

    Order* order = pool.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{50},
        Timestamp{10}
    );

    ASSERT_NE(order, nullptr);

    EXPECT_EQ(pool.available(), 1);
    EXPECT_EQ(pool.in_use(), 1);

    EXPECT_TRUE(pool.release(*order));

    EXPECT_EQ(pool.available(), 2);
    EXPECT_EQ(pool.in_use(), 0);
}


// ------------------------------------------------------------
// Released slot can be reused
// ------------------------------------------------------------

TEST(OrderPoolTest, ReleasedOrderSlotCanBeReused)
{
    OrderPool pool{1};

    Order* first = pool.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{50},
        Timestamp{10}
    );

    ASSERT_NE(first, nullptr);

    EXPECT_TRUE(pool.release(*first));

    Order* second = pool.acquire(
        2,
        Side::Sell,
        Price{101},
        Quantity{75},
        Timestamp{20}
    );

    ASSERT_NE(second, nullptr);

    EXPECT_EQ(second, first);

    EXPECT_EQ(second->id(), 2);
    EXPECT_EQ(second->side(), Side::Sell);
    EXPECT_EQ(second->price(), Price{101});
    EXPECT_EQ(second->quantity(), Quantity{75});
    EXPECT_EQ(second->remaining_quantity(), Quantity{75});
    EXPECT_EQ(second->timestamp(), Timestamp{20});
    EXPECT_EQ(second->state(), OrderState::New);
}


// ------------------------------------------------------------
// Released order is fully reinitialized
// ------------------------------------------------------------

TEST(OrderPoolTest, ReusedOrderIsFullyReset)
{
    OrderPool pool{1};

    Order* order = pool.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{100},
        Timestamp{10}
    );

    ASSERT_NE(order, nullptr);

    ASSERT_TRUE(order->activate());

    ASSERT_TRUE(
        order->execute(Quantity{40})
    );

    EXPECT_EQ(
        order->state(),
        OrderState::PartiallyFilled
    );

    EXPECT_EQ(
        order->remaining_quantity(),
        Quantity{60}
    );

    ASSERT_TRUE(pool.release(*order));

    Order* reused = pool.acquire(
        2,
        Side::Sell,
        Price{200},
        Quantity{300},
        Timestamp{20}
    );

    ASSERT_NE(reused, nullptr);

    EXPECT_EQ(reused->id(), 2);
    EXPECT_EQ(reused->side(), Side::Sell);
    EXPECT_EQ(reused->price(), Price{200});

    EXPECT_EQ(
        reused->quantity(),
        Quantity{300}
    );

    EXPECT_EQ(
        reused->remaining_quantity(),
        Quantity{300}
    );

    EXPECT_EQ(
        reused->state(),
        OrderState::New
    );

    EXPECT_EQ(
        reused->timestamp(),
        Timestamp{20}
    );
}


// ------------------------------------------------------------
// Release an order not owned by this pool
// ------------------------------------------------------------

TEST(OrderPoolTest, RejectsOrderFromAnotherPool)
{
    OrderPool pool1{1};
    OrderPool pool2{1};

    Order* order = pool1.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{50},
        Timestamp{10}
    );

    ASSERT_NE(order, nullptr);

    EXPECT_FALSE(pool2.release(*order));

    EXPECT_EQ(pool1.available(), 0);
    EXPECT_EQ(pool1.in_use(), 1);

    EXPECT_EQ(pool2.available(), 1);
    EXPECT_EQ(pool2.in_use(), 0);
}


// ------------------------------------------------------------
// Release invalid order
// ------------------------------------------------------------

TEST(OrderPoolTest, RejectsOrderNotOwnedByPool)
{
    OrderPool pool{1};

    Order external_order{
        100,
        Side::Buy,
        Price{100},
        Quantity{50},
        Timestamp{10}
    };

    EXPECT_FALSE(pool.release(external_order));

    EXPECT_EQ(pool.available(), 1);
    EXPECT_EQ(pool.in_use(), 0);
}


// ------------------------------------------------------------
// Multiple acquire/release operations
// ------------------------------------------------------------

TEST(OrderPoolTest, HandlesMultipleAcquireReleaseOperations)
{
    OrderPool pool{3};

    Order* order1 = pool.acquire(
        1,
        Side::Buy,
        Price{100},
        Quantity{10},
        Timestamp{1}
    );

    Order* order2 = pool.acquire(
        2,
        Side::Buy,
        Price{101},
        Quantity{20},
        Timestamp{2}
    );

    Order* order3 = pool.acquire(
        3,
        Side::Sell,
        Price{102},
        Quantity{30},
        Timestamp{3}
    );

    ASSERT_NE(order1, nullptr);
    ASSERT_NE(order2, nullptr);
    ASSERT_NE(order3, nullptr);

    EXPECT_EQ(pool.available(), 0);
    EXPECT_EQ(pool.in_use(), 3);

    EXPECT_TRUE(pool.release(*order2));

    EXPECT_EQ(pool.available(), 1);
    EXPECT_EQ(pool.in_use(), 2);

    Order* order4 = pool.acquire(
        4,
        Side::Sell,
        Price{103},
        Quantity{40},
        Timestamp{4}
    );

    ASSERT_NE(order4, nullptr);

    EXPECT_EQ(pool.available(), 0);
    EXPECT_EQ(pool.in_use(), 3);

    EXPECT_EQ(order4, order2);

    EXPECT_EQ(order4->id(), 4);
    EXPECT_EQ(order4->side(), Side::Sell);
    EXPECT_EQ(order4->price(), Price{103});
    EXPECT_EQ(order4->quantity(), Quantity{40});
    EXPECT_EQ(order4->remaining_quantity(), Quantity{40});
}

} // namespace
} // namespace simulator