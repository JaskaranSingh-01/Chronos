#include <gtest/gtest.h>

#include "simulator/orders/order.hpp"
#include "simulator/orders/price_level.hpp"

namespace simulator
{

    // Helper to create an Order.
    // Orders start in the New state, so we activate them before adding.
    Order make_order(
        OrderId id,
        Price price,
        Timestamp timestamp)
    {
        return Order{
            id,
            Side::Buy,
            price,
            Quantity{100},
            timestamp};
    }

    // ------------------------------------------------------------
    // Construction
    // ------------------------------------------------------------

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

    // ------------------------------------------------------------
    // Add
    // ------------------------------------------------------------

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

        // First order remains at the front.
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

        // Same order cannot be inserted again.
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

    // ------------------------------------------------------------
    // Remove - only order
    // ------------------------------------------------------------

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

    // ------------------------------------------------------------
    // Remove - head
    // ------------------------------------------------------------

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

        // Order 2 becomes the new head.
        EXPECT_EQ(level.front(), &order2);
    }

    // ------------------------------------------------------------
    // Remove - middle
    // ------------------------------------------------------------

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

        // Head is still order1.
        EXPECT_EQ(level.front(), &order1);
    }

    // ------------------------------------------------------------
    // Remove - tail
    // ------------------------------------------------------------

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

    // ------------------------------------------------------------
    // Remove invalid order
    // ------------------------------------------------------------

    TEST(PriceLevelTest, RejectsRemovingOrderThatDoesNotBelong)
    {
        PriceLevel level{Price{100}};

        auto order1 = make_order(1, Price{100}, 10);
        auto order2 = make_order(2, Price{100}, 20);

        ASSERT_TRUE(order1.activate());
        ASSERT_TRUE(order2.activate());

        ASSERT_TRUE(level.add(order1));

        // order2 was never added to this level.
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

        // order belongs to level1, not level2.
        EXPECT_FALSE(level2.remove(order));

        EXPECT_EQ(level1.order_count(), 1);
        EXPECT_EQ(level1.front(), &order);

        EXPECT_TRUE(level2.empty());
    }

    // ------------------------------------------------------------
    // Repeated removal
    // ------------------------------------------------------------

    TEST(PriceLevelTest, RejectsRemovingSameOrderTwice)
    {
        PriceLevel level{Price{100}};

        auto order = make_order(1, Price{100}, 10);
        ASSERT_TRUE(order.activate());

        ASSERT_TRUE(level.add(order));

        EXPECT_TRUE(level.remove(order));

        // Already removed.
        EXPECT_FALSE(level.remove(order));

        EXPECT_EQ(level.order_count(), 0);
        EXPECT_TRUE(level.empty());
    }

    // ------------------------------------------------------------
    // Reuse after removal
    // ------------------------------------------------------------

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

    // ------------------------------------------------------------
    // FIFO behavior after removing orders
    // ------------------------------------------------------------

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

        // After removing order1, order3 should become head.
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

    // ------------------------------------------------------------
    // Count consistency
    // ------------------------------------------------------------

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
} // namespace simulator