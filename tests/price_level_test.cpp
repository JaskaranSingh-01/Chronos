#include <gtest/gtest.h>

#include "simulator/orders/order.hpp"
#include "simulator/orders/price_level.hpp"

namespace simulator {

    // A New buy order of 100; tests activate it before adding.
Order make_order(OrderId id, Price price, Timestamp timestamp)
{
    return Order{id, Side::Buy, price, Quantity{100}, timestamp};
}

TEST(PriceLevelTest, StartsEmpty)
{
    PriceLevel level{Price{100}};

    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.order_count(), 0);
    EXPECT_EQ(level.front(), nullptr);
}

TEST(PriceLevelTest, StoresPrice)
{
    PriceLevel level{Price{100}};

    EXPECT_EQ(level.price(), Price{100});
}

TEST(PriceLevelTest, AddFirstOrder)
{
    PriceLevel level{Price{100}};

    auto order = make_order(1, Price{100}, 10);
    ASSERT_TRUE(order.activate());

    EXPECT_TRUE(level.add(order));

    EXPECT_FALSE(level.empty());
    EXPECT_EQ(level.order_count(), 1);
    EXPECT_EQ(level.front(), &order);
}

TEST(PriceLevelTest, AddMultipleOrdersPreservesFIFO)
{
    PriceLevel level{Price{100}};

    auto order1 = make_order(1, Price{100}, 10);
    auto order2 = make_order(2, Price{100}, 20);
    auto order3 = make_order(3, Price{100}, 30);

    ASSERT_TRUE(order1.activate());
    ASSERT_TRUE(order2.activate());
    ASSERT_TRUE(order3.activate());

    EXPECT_TRUE(level.add(order1));
    EXPECT_TRUE(level.add(order2));
    EXPECT_TRUE(level.add(order3));

    EXPECT_EQ(level.order_count(), 3);

    EXPECT_EQ(level.front(), &order1);
}

TEST(PriceLevelTest, RejectsOrderWithDifferentPrice)
{
    PriceLevel level{Price{100}};

    auto order = make_order(1, Price{101}, 10);
    ASSERT_TRUE(order.activate());

    EXPECT_FALSE(level.add(order));

    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.order_count(), 0);
    EXPECT_EQ(level.front(), nullptr);
}

TEST(PriceLevelTest, RejectsAddingSameOrderTwice)
{
    PriceLevel level{Price{100}};

    auto order = make_order(1, Price{100}, 10);
    ASSERT_TRUE(order.activate());

    EXPECT_TRUE(level.add(order));

    EXPECT_FALSE(level.add(order));

    EXPECT_EQ(level.order_count(), 1);
    EXPECT_EQ(level.front(), &order);
}

TEST(PriceLevelTest, OrderCanBeAddedToAnotherLevelAfterRemoval)
{
    PriceLevel level1{Price{100}};
    PriceLevel level2{Price{100}};

    auto order = make_order(1, Price{100}, 10);
    ASSERT_TRUE(order.activate());

    EXPECT_TRUE(level1.add(order));

    EXPECT_FALSE(level2.add(order));

    EXPECT_TRUE(level1.remove(order));

        // After removal the order is no longer owned by level1.
    EXPECT_TRUE(level2.add(order));

    EXPECT_EQ(level2.order_count(), 1);
    EXPECT_EQ(level2.front(), &order);
}

TEST(PriceLevelTest, RemoveOnlyOrder)
{
    PriceLevel level{Price{100}};

    auto order = make_order(1, Price{100}, 10);
    ASSERT_TRUE(order.activate());

    ASSERT_TRUE(level.add(order));

    EXPECT_TRUE(level.remove(order));

    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.order_count(), 0);
    EXPECT_EQ(level.front(), nullptr);
}

TEST(PriceLevelTest, RemoveHeadOrder)
{
    PriceLevel level{Price{100}};

    auto order1 = make_order(1, Price{100}, 10);
    auto order2 = make_order(2, Price{100}, 20);
    auto order3 = make_order(3, Price{100}, 30);

    ASSERT_TRUE(order1.activate());
    ASSERT_TRUE(order2.activate());
    ASSERT_TRUE(order3.activate());

    ASSERT_TRUE(level.add(order1));
    ASSERT_TRUE(level.add(order2));
    ASSERT_TRUE(level.add(order3));

    EXPECT_TRUE(level.remove(order1));

    EXPECT_EQ(level.order_count(), 2);

    EXPECT_EQ(level.front(), &order2);
}

TEST(PriceLevelTest, RemoveMiddleOrder)
{
    PriceLevel level{Price{100}};

    auto order1 = make_order(1, Price{100}, 10);
    auto order2 = make_order(2, Price{100}, 20);
    auto order3 = make_order(3, Price{100}, 30);

    ASSERT_TRUE(order1.activate());
    ASSERT_TRUE(order2.activate());
    ASSERT_TRUE(order3.activate());

    ASSERT_TRUE(level.add(order1));
    ASSERT_TRUE(level.add(order2));
    ASSERT_TRUE(level.add(order3));

    EXPECT_TRUE(level.remove(order2));

    EXPECT_EQ(level.order_count(), 2);

    EXPECT_EQ(level.front(), &order1);
}

TEST(PriceLevelTest, RemoveTailOrder)
{
    PriceLevel level{Price{100}};

    auto order1 = make_order(1, Price{100}, 10);
    auto order2 = make_order(2, Price{100}, 20);
    auto order3 = make_order(3, Price{100}, 30);

    ASSERT_TRUE(order1.activate());
    ASSERT_TRUE(order2.activate());
    ASSERT_TRUE(order3.activate());

    ASSERT_TRUE(level.add(order1));
    ASSERT_TRUE(level.add(order2));
    ASSERT_TRUE(level.add(order3));

    EXPECT_TRUE(level.remove(order3));

    EXPECT_EQ(level.order_count(), 2);

    EXPECT_EQ(level.front(), &order1);
}

TEST(PriceLevelTest, RejectsRemovingOrderThatDoesNotBelong)
{
    PriceLevel level{Price{100}};

    auto order1 = make_order(1, Price{100}, 10);
    auto order2 = make_order(2, Price{100}, 20);

    ASSERT_TRUE(order1.activate());
    ASSERT_TRUE(order2.activate());

    ASSERT_TRUE(level.add(order1));

    EXPECT_FALSE(level.remove(order2));

    EXPECT_EQ(level.order_count(), 1);
    EXPECT_EQ(level.front(), &order1);
}

TEST(PriceLevelTest, RejectsRemovingOrderFromAnotherLevel)
{
    PriceLevel level1{Price{100}};
    PriceLevel level2{Price{100}};

    auto order = make_order(1, Price{100}, 10);
    ASSERT_TRUE(order.activate());

    ASSERT_TRUE(level1.add(order));

    EXPECT_FALSE(level2.remove(order));

    EXPECT_EQ(level1.order_count(), 1);
    EXPECT_EQ(level1.front(), &order);

    EXPECT_TRUE(level2.empty());
}

TEST(PriceLevelTest, RejectsRemovingSameOrderTwice)
{
    PriceLevel level{Price{100}};

    auto order = make_order(1, Price{100}, 10);
    ASSERT_TRUE(order.activate());

    ASSERT_TRUE(level.add(order));

    EXPECT_TRUE(level.remove(order));

    EXPECT_FALSE(level.remove(order));

    EXPECT_EQ(level.order_count(), 0);
    EXPECT_TRUE(level.empty());
}

TEST(PriceLevelTest, CanReAddOrderAfterRemoval)
{
    PriceLevel level{Price{100}};

    auto order = make_order(1, Price{100}, 10);
    ASSERT_TRUE(order.activate());

    ASSERT_TRUE(level.add(order));

    EXPECT_TRUE(level.remove(order));

    EXPECT_TRUE(level.add(order));

    EXPECT_EQ(level.order_count(), 1);
    EXPECT_EQ(level.front(), &order);
}

TEST(PriceLevelTest, FIFOIsPreservedAfterRemovingMiddleOrder)
{
    PriceLevel level{Price{100}};

    auto order1 = make_order(1, Price{100}, 10);
    auto order2 = make_order(2, Price{100}, 20);
    auto order3 = make_order(3, Price{100}, 30);
    auto order4 = make_order(4, Price{100}, 40);

    ASSERT_TRUE(order1.activate());
    ASSERT_TRUE(order2.activate());
    ASSERT_TRUE(order3.activate());
    ASSERT_TRUE(order4.activate());

    ASSERT_TRUE(level.add(order1));
    ASSERT_TRUE(level.add(order2));
    ASSERT_TRUE(level.add(order3));
    ASSERT_TRUE(level.add(order4));

    ASSERT_TRUE(level.remove(order2));

    EXPECT_EQ(level.front(), &order1);

    ASSERT_TRUE(level.remove(order1));

    EXPECT_EQ(level.front(), &order3);
}

TEST(PriceLevelTest, FIFOIsPreservedAfterRemovingHead)
{
    PriceLevel level{Price{100}};

    auto order1 = make_order(1, Price{100}, 10);
    auto order2 = make_order(2, Price{100}, 20);
    auto order3 = make_order(3, Price{100}, 30);

    ASSERT_TRUE(order1.activate());
    ASSERT_TRUE(order2.activate());
    ASSERT_TRUE(order3.activate());

    ASSERT_TRUE(level.add(order1));
    ASSERT_TRUE(level.add(order2));
    ASSERT_TRUE(level.add(order3));

    ASSERT_TRUE(level.remove(order1));

    EXPECT_EQ(level.front(), &order2);

    ASSERT_TRUE(level.remove(order2));

    EXPECT_EQ(level.front(), &order3);
}

TEST(PriceLevelTest, OrderCountRemainsConsistent)
{
    PriceLevel level{Price{100}};

    auto order1 = make_order(1, Price{100}, 10);
    auto order2 = make_order(2, Price{100}, 20);
    auto order3 = make_order(3, Price{100}, 30);

    ASSERT_TRUE(order1.activate());
    ASSERT_TRUE(order2.activate());
    ASSERT_TRUE(order3.activate());

    EXPECT_EQ(level.order_count(), 0);

    ASSERT_TRUE(level.add(order1));
    EXPECT_EQ(level.order_count(), 1);

    ASSERT_TRUE(level.add(order2));
    EXPECT_EQ(level.order_count(), 2);

    ASSERT_TRUE(level.add(order3));
    EXPECT_EQ(level.order_count(), 3);

    ASSERT_TRUE(level.remove(order2));
    EXPECT_EQ(level.order_count(), 2);

    ASSERT_TRUE(level.remove(order1));
    EXPECT_EQ(level.order_count(), 1);

    ASSERT_TRUE(level.remove(order3));
    EXPECT_EQ(level.order_count(), 0);

    EXPECT_TRUE(level.empty());
}

TEST(PriceLevelTest, TracksTotalQuantity)
{
    PriceLevel level{Price{100}};

    auto order1 = make_order(1, Price{100}, 10);
    auto order2 = make_order(2, Price{100}, 20);

    ASSERT_TRUE(order1.activate());
    ASSERT_TRUE(order2.activate());

    EXPECT_EQ(level.total_quantity(), 0u);

    ASSERT_TRUE(level.add(order1));
    ASSERT_TRUE(level.add(order2));
    EXPECT_EQ(level.total_quantity(), 200u);

        // A partial change goes through reduce(); remove() then subtracts
        // whatever is remaining at unlink time.
    ASSERT_TRUE(order1.execute(Quantity{30}));
    level.reduce(Quantity{30});
    EXPECT_EQ(level.total_quantity(), 170u);

    ASSERT_TRUE(level.remove(order1));
    EXPECT_EQ(level.total_quantity(), 100u);

    ASSERT_TRUE(level.remove(order2));
    EXPECT_EQ(level.total_quantity(), 0u);
}
} // namespace simulator