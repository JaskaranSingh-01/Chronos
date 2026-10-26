#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "simulator/orders/order_book.hpp"

namespace simulator {
namespace {

Order make_buy_order(OrderId id, Price price, Quantity quantity = Quantity{100}, Timestamp timestamp = 1)
{
    return Order{id, Side::Buy, price, quantity, timestamp};
}

Order make_sell_order(OrderId id, Price price, Quantity quantity = Quantity{100}, Timestamp timestamp = 1)
{
    return Order{id, Side::Sell, price, quantity, timestamp};
}

void activate(Order& order)
{
    ASSERT_TRUE(order.activate());
}

class OrderBookTest : public testing::Test
{
protected:
    BookStorage storage{64};
    OrderBook book{0, storage};

    // The prices of `side`'s levels, best first, each checked against its neighbours' links.
    std::vector<std::int64_t> prices(Side side) const
    {
        std::vector<std::int64_t> out;
        const PriceLevel* before = nullptr;
        for (const PriceLevel* level = book.best(side); level != nullptr; before = level, level = level->worse()) {
            EXPECT_EQ(level->better(), before);
            out.push_back(level->price().ticks);
        }
        return out;
    }
};

} // namespace

TEST_F(OrderBookTest, StartsEmpty)
{
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask(), nullptr);
}

TEST_F(OrderBookTest, AddsActiveBuyOrder)
{
    auto order = make_buy_order(1, Price{100});
    activate(order);

    EXPECT_TRUE(book.add(order));

    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.find(1), &order);
    EXPECT_EQ(book.best_bid(), &order);
}

TEST_F(OrderBookTest, AddsActiveSellOrder)
{
    auto order = make_sell_order(1, Price{101});
    activate(order);

    EXPECT_TRUE(book.add(order));

    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.find(1), &order);
    EXPECT_EQ(book.best_ask(), &order);
}

TEST_F(OrderBookTest, RejectsInactiveOrder)
{
    auto order = make_buy_order(1, Price{100});

    EXPECT_EQ(order.state(), OrderState::New);
    EXPECT_FALSE(book.add(order));

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.find(1), nullptr);
}

TEST_F(OrderBookTest, RejectsDuplicateOrderId)
{
    auto first = make_buy_order(1, Price{100});
    auto second = make_buy_order(1, Price{101});

    activate(first);
    activate(second);

    EXPECT_TRUE(book.add(first));
    EXPECT_FALSE(book.add(second));

    EXPECT_EQ(book.find(1), &first);
    EXPECT_EQ(book.best_bid(), &first);
}

// Order::activate accepts a zero quantity, so every add path relies on this
// guard: nothing may rest with 0 shares.
TEST_F(OrderBookTest, RejectsZeroQuantityOrder)
{
    auto order = make_sell_order(1, Price{100}, Quantity{0});
    activate(order);

    EXPECT_FALSE(book.add(order));

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.best(Side::Sell), nullptr);  // no empty level left behind
    EXPECT_EQ(order.level(), nullptr);
}

TEST_F(OrderBookTest, FindsExistingOrder)
{
    auto order = make_buy_order(42, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    EXPECT_EQ(book.find(42), &order);
}

TEST_F(OrderBookTest, ReturnsNullForMissingOrder)
{
    EXPECT_EQ(book.find(999), nullptr);
}

TEST_F(OrderBookTest, ConstFindWorks)
{
    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    const OrderBook& const_book = book;

    EXPECT_EQ(const_book.find(1), &order);
    EXPECT_EQ(const_book.find(999), nullptr);
}

TEST_F(OrderBookTest, BestBidIsHighestPrice)
{
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

TEST_F(OrderBookTest, BestBidUpdatesAfterCancellation)
{
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

TEST_F(OrderBookTest, BestAskIsLowestPrice)
{
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

TEST_F(OrderBookTest, BestAskUpdatesAfterCancellation)
{
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

TEST_F(OrderBookTest, BuyOrdersAtSamePricePreserveFIFO)
{
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

TEST_F(OrderBookTest, SellOrdersAtSamePricePreserveFIFO)
{
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

TEST_F(OrderBookTest, BuyAndSellOrdersCanCoexist)
{
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

TEST_F(OrderBookTest, CancelsOrder)
{
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

TEST_F(OrderBookTest, CancelsMissingOrder)
{
    EXPECT_FALSE(book.cancel(999));
}

TEST_F(OrderBookTest, CancelChangesOrderState)
{
    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    EXPECT_EQ(order.state(), OrderState::Active);

    ASSERT_TRUE(book.cancel(order.id()));

    EXPECT_EQ(order.state(), OrderState::Cancelled);
}

TEST_F(OrderBookTest, CannotCancelOrderTwice)
{
    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    ASSERT_TRUE(book.cancel(order.id()));

    EXPECT_FALSE(book.cancel(order.id()));
}

TEST_F(OrderBookTest, CannotCancelInactiveOrder)
{
    auto order = make_buy_order(1, Price{100});

    EXPECT_EQ(order.state(), OrderState::New);

    EXPECT_FALSE(book.cancel(order.id()));
}

TEST_F(OrderBookTest, PartiallyFilledOrderCanBeCancelled)
{
    auto order = make_buy_order(1, Price{100}, Quantity{100});

    activate(order);

    ASSERT_TRUE(book.add(order));

    ASSERT_TRUE(order.execute(Quantity{40}));

    EXPECT_EQ(order.state(), OrderState::PartiallyFilled);

    EXPECT_EQ(order.remaining_quantity(), Quantity{60});

    EXPECT_TRUE(book.cancel(order.id()));

    EXPECT_EQ(order.state(), OrderState::Cancelled);
    EXPECT_EQ(book.find(order.id()), nullptr);
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, CancellingOnlyOrderRemovesPriceLevel)
{
    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    ASSERT_TRUE(book.cancel(order.id()));

    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, CancellingBestBidRevealsNextPrice)
{
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

TEST_F(OrderBookTest, CancellingBestAskRevealsNextPrice)
{
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

TEST_F(OrderBookTest, CancellingFirstBuyOrderPreservesRemainingFIFO)
{
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

TEST_F(OrderBookTest, CancellingMiddleBuyOrderPreservesFIFO)
{
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

TEST_F(OrderBookTest, CompleteOrderLifecycle)
{
    auto order = make_buy_order(1, Price{100}, Quantity{100});

    EXPECT_EQ(order.state(), OrderState::New);

    ASSERT_TRUE(order.activate());
    EXPECT_EQ(order.state(), OrderState::Active);

    ASSERT_TRUE(book.add(order));

    EXPECT_EQ(book.find(order.id()), &order);
    EXPECT_EQ(book.best_bid(), &order);

    ASSERT_TRUE(order.execute(Quantity{40}));

    EXPECT_EQ(order.state(), OrderState::PartiallyFilled);

    EXPECT_EQ(order.remaining_quantity(), Quantity{60});

    EXPECT_EQ(book.find(order.id()), &order);
    EXPECT_EQ(book.best_bid(), &order);

    ASSERT_TRUE(book.cancel(order.id()));

    EXPECT_EQ(order.state(), OrderState::Cancelled);
    EXPECT_EQ(book.find(order.id()), nullptr);
    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, HandlesMultipleOrdersAcrossMultiplePriceLevels)
{
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

TEST_F(OrderBookTest, ConstBestBidWorks)
{
    auto order = make_buy_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    const OrderBook& const_book = book;

    EXPECT_EQ(const_book.best_bid(), &order);
}

TEST_F(OrderBookTest, ConstBestAskWorks)
{
    auto order = make_sell_order(1, Price{100});
    activate(order);

    ASSERT_TRUE(book.add(order));

    const OrderBook& const_book = book;

    EXPECT_EQ(const_book.best_ask(), &order);
}

TEST_F(OrderBookTest, LevelTotalAfterPartialExecute)
{
    auto order1 = make_buy_order(1, Price{100}, Quantity{100});
    auto order2 = make_buy_order(2, Price{100}, Quantity{50});
    activate(order1);
    activate(order2);
    ASSERT_TRUE(book.add(order1));
    ASSERT_TRUE(book.add(order2));

    const PriceLevel& level = *book.level(Side::Buy, Price{100});
    EXPECT_EQ(level.total_quantity(), 150u);

    ASSERT_TRUE(book.execute(1, Quantity{40}));
    EXPECT_EQ(level.total_quantity(), 110u);

    // A full execute unlinks the order; the level keeps the other one.
    ASSERT_TRUE(book.execute(1, Quantity{60}));
    EXPECT_EQ(level.total_quantity(), 50u);
    EXPECT_EQ(level.order_count(), 1u);
}

TEST_F(OrderBookTest, LevelTotalAfterPartialCancel)
{
    auto order1 = make_sell_order(1, Price{100}, Quantity{100});
    auto order2 = make_sell_order(2, Price{100}, Quantity{50});
    activate(order1);
    activate(order2);
    ASSERT_TRUE(book.add(order1));
    ASSERT_TRUE(book.add(order2));

    const PriceLevel& level = *book.level(Side::Sell, Price{100});

    ASSERT_TRUE(book.cancel(2, Quantity{20}));
    EXPECT_EQ(level.total_quantity(), 130u);

    ASSERT_TRUE(book.cancel(2, Quantity{30}));
    EXPECT_EQ(level.total_quantity(), 100u);
    EXPECT_EQ(level.order_count(), 1u);
}

TEST_F(OrderBookTest, LevelTotalAfterErase)
{
    auto order1 = make_buy_order(1, Price{100}, Quantity{100});
    auto order2 = make_buy_order(2, Price{100}, Quantity{50});
    activate(order1);
    activate(order2);
    ASSERT_TRUE(book.add(order1));
    ASSERT_TRUE(book.add(order2));
    ASSERT_TRUE(book.execute(2, Quantity{10}));

    const PriceLevel& level = *book.level(Side::Buy, Price{100});

    ASSERT_TRUE(book.erase(2));
    EXPECT_EQ(level.total_quantity(), 100u);
    EXPECT_EQ(book.size(), 1u);

    ASSERT_TRUE(book.erase(1));
    EXPECT_EQ(book.best(Side::Buy), nullptr);
    EXPECT_EQ(book.size(), 0u);
}

TEST_F(OrderBookTest, LevelsStayBestFirstWhereverANewOneGoes)
{
    std::vector<Order> orders;
    orders.reserve(10);
    for (const std::int64_t price : {100, 102, 101, 99, 103}) {
        orders.push_back(make_buy_order(static_cast<OrderId>(orders.size() + 1), Price{price}));
        orders.push_back(make_sell_order(static_cast<OrderId>(orders.size() + 1), Price{price + 10}));
    }
    for (Order& order : orders) {
        activate(order);
        ASSERT_TRUE(book.add(order));
    }

    EXPECT_EQ(prices(Side::Buy), (std::vector<std::int64_t>{103, 102, 101, 100, 99}));
    EXPECT_EQ(prices(Side::Sell), (std::vector<std::int64_t>{109, 110, 111, 112, 113}));
    EXPECT_EQ(book.level_count(), 10u);
    EXPECT_EQ(storage.levels.in_use(), 10u);
    EXPECT_EQ(storage.level_index.size(), 10u);
}

TEST_F(OrderBookTest, AnEmptiedLevelLeavesItsListAndTheStorage)
{
    std::vector<Order> orders;
    orders.reserve(5);
    for (const std::int64_t price : {100, 101, 102, 103, 101}) {
        orders.push_back(make_buy_order(static_cast<OrderId>(orders.size() + 1), Price{price}));
        activate(orders.back());
        ASSERT_TRUE(book.add(orders.back()));
    }

    ASSERT_TRUE(book.erase(3));  // the middle level, 102
    EXPECT_EQ(prices(Side::Buy), (std::vector<std::int64_t>{103, 101, 100}));
    ASSERT_TRUE(book.erase(4));  // the best, 103
    EXPECT_EQ(prices(Side::Buy), (std::vector<std::int64_t>{101, 100}));
    EXPECT_EQ(book.best_bid(), &orders[1]);
    ASSERT_TRUE(book.erase(1));  // the worst, 100
    ASSERT_TRUE(book.erase(2));  // 101 keeps order 5
    EXPECT_EQ(prices(Side::Buy), (std::vector<std::int64_t>{101}));
    EXPECT_EQ(book.level(Side::Buy, Price{102}), nullptr);
    EXPECT_EQ(storage.levels.in_use(), 1u);
    EXPECT_EQ(storage.level_index.size(), 1u);
    EXPECT_EQ(storage.order_index.size(), 1u);

    ASSERT_TRUE(book.erase(5));
    EXPECT_EQ(book.best(Side::Buy), nullptr);
    EXPECT_EQ(storage.levels.in_use(), 0u);
    EXPECT_EQ(storage.level_index.size(), 0u);
}

TEST_F(OrderBookTest, RoomNamesTheFirstFullStructure)
{
    BookStorage small{BookCapacity{.orders = 0, .order_index = 8, .levels = 2, .level_index = 8}};
    OrderBook tight{1, small};
    std::vector<Order> orders;
    orders.reserve(9);
    for (OrderId id = 1; id <= 9; ++id) {
        orders.push_back(make_buy_order(id, Price{id <= 7 ? 100 : 101}));
        activate(orders.back());
    }

    for (OrderId id = 1; id <= 7; ++id) {
        ASSERT_TRUE(tight.add(orders[id - 1]));
    }
    EXPECT_EQ(tight.room(Side::Buy, Price{101}), BookStructure::None);
    ASSERT_TRUE(tight.add(orders[7]));  // the second level
    EXPECT_EQ(tight.room(Side::Buy, Price{102}), BookStructure::OrderIndex);
    EXPECT_FALSE(tight.add(orders[8]));
    EXPECT_EQ(tight.size(), 8u);

    ASSERT_TRUE(tight.erase(1));
    EXPECT_EQ(tight.room(Side::Buy, Price{100}), BookStructure::None);
    EXPECT_EQ(tight.room(Side::Buy, Price{102}), BookStructure::LevelPool);
    EXPECT_EQ(tight.room(Side::Buy, Price{102}, &orders[7]), BookStructure::None);  // 101 empties first
    EXPECT_EQ(tight.room(Side::Buy, Price{102}, &orders[1]), BookStructure::LevelPool);
    EXPECT_EQ(book.room(Side::Buy, Price{102}), BookStructure::None);  // another storage
}

} // namespace simulator
