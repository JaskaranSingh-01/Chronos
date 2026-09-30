#include <gtest/gtest.h>

#include "simulator/orders/order_book.hpp"

namespace simulator {
namespace {

// -----------------------------------------------------------------------------
// Test helpers
// -----------------------------------------------------------------------------

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


// =============================================================================
// Basic state
// =============================================================================

TEST(OrderBookTest, StartsEmpty)
{
    OrderBook book;

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask(), nullptr);
}


// =============================================================================
// Adding orders
// =============================================================================

TEST(OrderBookTest, AddsActiveBuyOrder)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});
    activate(order);

    EXPECT_TRUE(book.add(order));

    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.find(1), &order);
    EXPECT_EQ(book.best_bid(), &order);
}

TEST(OrderBookTest, AddsActiveSellOrder)
{
    OrderBook book;

    auto order = make_sell_order(1, Price{101});
    activate(order);

    EXPECT_TRUE(book.add(order));

    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.find(1), &order);
    EXPECT_EQ(book.best_ask(), &order);
}

TEST(OrderBookTest, RejectsInactiveOrder)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});

    EXPECT_EQ(order.state(), OrderState::New);
    EXPECT_FALSE(book.add(order));

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.find(1), nullptr);
}

TEST(OrderBookTest, RejectsDuplicateOrderId)
{
    OrderBook book;

    auto first = make_buy_order(1, Price{100});
    auto second = make_buy_order(1, Price{101});

    activate(first);
    activate(second);

    EXPECT_TRUE(book.add(first));
    EXPECT_FALSE(book.add(second));

    EXPECT_EQ(book.find(1), &first);
    EXPECT_EQ(book.best_bid(), &first);
}


// =============================================================================
// Order lookup
// =============================================================================

TEST(OrderBookTest, FindsExistingOrder)
{
    OrderBook book;

    auto order = make_buy_order(42, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    EXPECT_EQ(book.find(42), &order);
}

TEST(OrderBookTest, ReturnsNullForMissingOrder)
{
    OrderBook book;

    EXPECT_EQ(book.find(999), nullptr);
}

TEST(OrderBookTest, ConstFindWorks)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    const OrderBook& const_book = book;

    EXPECT_EQ(const_book.find(1), &order);
    EXPECT_EQ(const_book.find(999), nullptr);
}


// =============================================================================
// Bid price priority
// =============================================================================

TEST(OrderBookTest, BestBidIsHighestPrice)
{
    OrderBook book;

    auto low = make_buy_order(1, Price{100});
    auto high = make_buy_order(2, Price{105});
    auto middle = make_buy_order(3, Price{102});

    activate(low);
    activate(high);
    activate(middle);

    ASSERT_TRUE(book.add(low));
    ASSERT_TRUE(book.add(high));
    ASSERT_TRUE(book.add(middle));

    EXPECT_EQ(book.best_bid(), &high);
}

TEST(OrderBookTest, BestBidUpdatesAfterCancellation)
{
    OrderBook book;

    auto low = make_buy_order(1, Price{100});
    auto high = make_buy_order(2, Price{105});

    activate(low);
    activate(high);

    ASSERT_TRUE(book.add(low));
    ASSERT_TRUE(book.add(high));

    EXPECT_EQ(book.best_bid(), &high);

    ASSERT_TRUE(book.cancel(high.id()));

    EXPECT_EQ(book.best_bid(), &low);
}


// =============================================================================
// Ask price priority
// =============================================================================

TEST(OrderBookTest, BestAskIsLowestPrice)
{
    OrderBook book;

    auto high = make_sell_order(1, Price{105});
    auto low = make_sell_order(2, Price{100});
    auto middle = make_sell_order(3, Price{102});

    activate(high);
    activate(low);
    activate(middle);

    ASSERT_TRUE(book.add(high));
    ASSERT_TRUE(book.add(low));
    ASSERT_TRUE(book.add(middle));

    EXPECT_EQ(book.best_ask(), &low);
}

TEST(OrderBookTest, BestAskUpdatesAfterCancellation)
{
    OrderBook book;

    auto low = make_sell_order(1, Price{100});
    auto high = make_sell_order(2, Price{105});

    activate(low);
    activate(high);

    ASSERT_TRUE(book.add(low));
    ASSERT_TRUE(book.add(high));

    EXPECT_EQ(book.best_ask(), &low);

    ASSERT_TRUE(book.cancel(low.id()));

    EXPECT_EQ(book.best_ask(), &high);
}


// =============================================================================
// FIFO priority at the same price
// =============================================================================

TEST(OrderBookTest, BuyOrdersAtSamePricePreserveFIFO)
{
    OrderBook book;

    auto first = make_buy_order(1, Price{100}, Quantity{100}, 1);
    auto second = make_buy_order(2, Price{100}, Quantity{100}, 2);
    auto third = make_buy_order(3, Price{100}, Quantity{100}, 3);

    activate(first);
    activate(second);
    activate(third);

    ASSERT_TRUE(book.add(first));
    ASSERT_TRUE(book.add(second));
    ASSERT_TRUE(book.add(third));

    EXPECT_EQ(book.best_bid(), &first);
}

TEST(OrderBookTest, SellOrdersAtSamePricePreserveFIFO)
{
    OrderBook book;

    auto first = make_sell_order(1, Price{100}, Quantity{100}, 1);
    auto second = make_sell_order(2, Price{100}, Quantity{100}, 2);
    auto third = make_sell_order(3, Price{100}, Quantity{100}, 3);

    activate(first);
    activate(second);
    activate(third);

    ASSERT_TRUE(book.add(first));
    ASSERT_TRUE(book.add(second));
    ASSERT_TRUE(book.add(third));

    EXPECT_EQ(book.best_ask(), &first);
}


// =============================================================================
// Buy and sell books are independent
// =============================================================================

TEST(OrderBookTest, BuyAndSellOrdersCanCoexist)
{
    OrderBook book;

    auto buy = make_buy_order(1, Price{100});
    auto sell = make_sell_order(2, Price{101});

    activate(buy);
    activate(sell);

    ASSERT_TRUE(book.add(buy));
    ASSERT_TRUE(book.add(sell));

    EXPECT_EQ(book.best_bid(), &buy);
    EXPECT_EQ(book.best_ask(), &sell);

    EXPECT_EQ(book.find(1), &buy);
    EXPECT_EQ(book.find(2), &sell);
}


// =============================================================================
// Cancellation
// =============================================================================

TEST(OrderBookTest, CancelsOrder)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    EXPECT_EQ(book.find(1), &order);
    EXPECT_EQ(book.best_bid(), &order);

    EXPECT_TRUE(book.cancel(1));

    EXPECT_EQ(book.find(1), nullptr);
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, CancelsMissingOrder)
{
    OrderBook book;

    EXPECT_FALSE(book.cancel(999));
}

TEST(OrderBookTest, CancelChangesOrderState)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    EXPECT_EQ(order.state(), OrderState::Active);

    ASSERT_TRUE(book.cancel(order.id()));

    EXPECT_EQ(order.state(), OrderState::Cancelled);
}

TEST(OrderBookTest, CannotCancelOrderTwice)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    ASSERT_TRUE(book.cancel(order.id()));

    EXPECT_FALSE(book.cancel(order.id()));
}

TEST(OrderBookTest, CannotCancelInactiveOrder)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});

    // Order was never activated.
    EXPECT_EQ(order.state(), OrderState::New);

    EXPECT_FALSE(book.cancel(order.id()));
}

TEST(OrderBookTest, PartiallyFilledOrderCanBeCancelled)
{
    OrderBook book;

    auto order = make_buy_order(
        1,
        Price{100},
        Quantity{100}
    );

    activate(order);

    ASSERT_TRUE(book.add(order));

    ASSERT_TRUE(order.execute(Quantity{40}));

    EXPECT_EQ(
        order.state(),
        OrderState::PartiallyFilled
    );

    EXPECT_EQ(
        order.remaining_quantity(),
        Quantity{60}
    );

    EXPECT_TRUE(book.cancel(order.id()));

    EXPECT_EQ(order.state(), OrderState::Cancelled);
    EXPECT_EQ(book.find(order.id()), nullptr);
    EXPECT_TRUE(book.empty());
}


// =============================================================================
// Price-level cleanup
// =============================================================================

TEST(OrderBookTest, CancellingOnlyOrderRemovesPriceLevel)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    ASSERT_TRUE(book.cancel(order.id()));

    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, CancellingBestBidRevealsNextPrice)
{
    OrderBook book;

    auto best = make_buy_order(1, Price{105});
    auto next = make_buy_order(2, Price{100});

    activate(best);
    activate(next);

    ASSERT_TRUE(book.add(best));
    ASSERT_TRUE(book.add(next));

    EXPECT_EQ(book.best_bid(), &best);

    ASSERT_TRUE(book.cancel(best.id()));

    EXPECT_EQ(book.best_bid(), &next);
}

TEST(OrderBookTest, CancellingBestAskRevealsNextPrice)
{
    OrderBook book;

    auto best = make_sell_order(1, Price{100});
    auto next = make_sell_order(2, Price{105});

    activate(best);
    activate(next);

    ASSERT_TRUE(book.add(best));
    ASSERT_TRUE(book.add(next));

    EXPECT_EQ(book.best_ask(), &best);

    ASSERT_TRUE(book.cancel(best.id()));

    EXPECT_EQ(book.best_ask(), &next);
}


// =============================================================================
// FIFO after cancellation
// =============================================================================

TEST(OrderBookTest, CancellingFirstBuyOrderPreservesRemainingFIFO)
{
    OrderBook book;

    auto first = make_buy_order(1, Price{100});
    auto second = make_buy_order(2, Price{100});
    auto third = make_buy_order(3, Price{100});

    activate(first);
    activate(second);
    activate(third);

    ASSERT_TRUE(book.add(first));
    ASSERT_TRUE(book.add(second));
    ASSERT_TRUE(book.add(third));

    ASSERT_TRUE(book.cancel(first.id()));

    EXPECT_EQ(book.best_bid(), &second);
}

TEST(OrderBookTest, CancellingMiddleBuyOrderPreservesFIFO)
{
    OrderBook book;

    auto first = make_buy_order(1, Price{100});
    auto second = make_buy_order(2, Price{100});
    auto third = make_buy_order(3, Price{100});

    activate(first);
    activate(second);
    activate(third);

    ASSERT_TRUE(book.add(first));
    ASSERT_TRUE(book.add(second));
    ASSERT_TRUE(book.add(third));

    ASSERT_TRUE(book.cancel(second.id()));

    EXPECT_EQ(book.best_bid(), &first);

    ASSERT_TRUE(book.cancel(first.id()));

    EXPECT_EQ(book.best_bid(), &third);
}


// =============================================================================
// Complete lifecycle
// =============================================================================

TEST(OrderBookTest, CompleteOrderLifecycle)
{
    OrderBook book;

    auto order = make_buy_order(
        1,
        Price{100},
        Quantity{100}
    );

    // New
    EXPECT_EQ(order.state(), OrderState::New);

    // New -> Active
    ASSERT_TRUE(order.activate());
    EXPECT_EQ(order.state(), OrderState::Active);

    // Add to book
    ASSERT_TRUE(book.add(order));

    EXPECT_EQ(book.find(order.id()), &order);
    EXPECT_EQ(book.best_bid(), &order);

    // Partial execution
    ASSERT_TRUE(order.execute(Quantity{40}));

    EXPECT_EQ(
        order.state(),
        OrderState::PartiallyFilled
    );

    EXPECT_EQ(
        order.remaining_quantity(),
        Quantity{60}
    );

    // Order should still be resting in the book.
    EXPECT_EQ(book.find(order.id()), &order);
    EXPECT_EQ(book.best_bid(), &order);

    // Cancel remaining quantity
    ASSERT_TRUE(book.cancel(order.id()));

    EXPECT_EQ(order.state(), OrderState::Cancelled);
    EXPECT_EQ(book.find(order.id()), nullptr);
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_TRUE(book.empty());
}


// =============================================================================
// Multiple orders and both sides
// =============================================================================

TEST(OrderBookTest, HandlesMultipleOrdersAcrossMultiplePriceLevels)
{
    OrderBook book;

    auto bid1 = make_buy_order(1, Price{100});
    auto bid2 = make_buy_order(2, Price{105});
    auto bid3 = make_buy_order(3, Price{102});

    auto ask1 = make_sell_order(4, Price{110});
    auto ask2 = make_sell_order(5, Price{101});
    auto ask3 = make_sell_order(6, Price{105});

    activate(bid1);
    activate(bid2);
    activate(bid3);

    activate(ask1);
    activate(ask2);
    activate(ask3);

    ASSERT_TRUE(book.add(bid1));
    ASSERT_TRUE(book.add(bid2));
    ASSERT_TRUE(book.add(bid3));

    ASSERT_TRUE(book.add(ask1));
    ASSERT_TRUE(book.add(ask2));
    ASSERT_TRUE(book.add(ask3));

    EXPECT_EQ(book.best_bid(), &bid2);
    EXPECT_EQ(book.best_ask(), &ask2);

    EXPECT_EQ(book.find(1), &bid1);
    EXPECT_EQ(book.find(2), &bid2);
    EXPECT_EQ(book.find(3), &bid3);

    EXPECT_EQ(book.find(4), &ask1);
    EXPECT_EQ(book.find(5), &ask2);
    EXPECT_EQ(book.find(6), &ask3);

    EXPECT_FALSE(book.empty());
}


// =============================================================================
// Const access
// =============================================================================

TEST(OrderBookTest, ConstBestBidWorks)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    const OrderBook& const_book = book;

    EXPECT_EQ(const_book.best_bid(), &order);
}

TEST(OrderBookTest, ConstBestAskWorks)
{
    OrderBook book;

    auto order = make_sell_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    const OrderBook& const_book = book;

    EXPECT_EQ(const_book.best_ask(), &order);
}

TEST(OrderBookTest, RemovesFilledOrder)
{
    OrderBook book;

    auto order = make_buy_order(
        1,
        Price{100},
        Quantity{100}
    );

    activate(order);

    ASSERT_TRUE(book.add(order));

    ASSERT_TRUE(order.execute(Quantity{100}));

    EXPECT_EQ(order.state(), OrderState::Filled);
    EXPECT_EQ(book.find(order.id()), &order);

    EXPECT_TRUE(book.remove(order.id()));

    EXPECT_EQ(book.find(order.id()), nullptr);
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_TRUE(book.empty());

    // Important: remove() does not change lifecycle state.
    EXPECT_EQ(order.state(), OrderState::Filled);
}



TEST(OrderBookTest, CannotRemoveActiveOrder)
{
    OrderBook book;

    auto order = make_buy_order(1, Price{100});

    activate(order);

    ASSERT_TRUE(book.add(order));

    EXPECT_FALSE(book.remove(order.id()));

    EXPECT_EQ(book.find(order.id()), &order);
    EXPECT_EQ(order.state(), OrderState::Active);
}

TEST(OrderBookTest, CannotRemovePartiallyFilledOrder)
{
    OrderBook book;

    auto order = make_buy_order(
        1,
        Price{100},
        Quantity{100}
    );

    activate(order);

    ASSERT_TRUE(book.add(order));

    ASSERT_TRUE(order.execute(Quantity{40}));

    EXPECT_EQ(
        order.state(),
        OrderState::PartiallyFilled
    );

    EXPECT_FALSE(book.remove(order.id()));

    EXPECT_EQ(book.find(order.id()), &order);
}



TEST(OrderBookTest, RemovesFilledSellOrder)
{
    OrderBook book;

    auto order = make_sell_order(
        1,
        Price{100},
        Quantity{100}
    );

    activate(order);

    ASSERT_TRUE(book.add(order));

    ASSERT_TRUE(order.execute(Quantity{100}));

    EXPECT_EQ(order.state(), OrderState::Filled);

    EXPECT_TRUE(book.remove(order.id()));

    EXPECT_EQ(book.find(order.id()), nullptr);
    EXPECT_EQ(book.best_ask(), nullptr);
    EXPECT_TRUE(book.empty());
}

} // namespace simulator