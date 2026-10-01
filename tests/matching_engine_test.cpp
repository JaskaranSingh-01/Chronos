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

TEST(MatchingEngineTest, PartialFillIncomingOrderLarger)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto sell = make_sell_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    activate(sell);
    ASSERT_TRUE(book.add(sell));

    auto buy = make_buy_order(
        2,
        Price{101},
        Quantity{150},
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

    EXPECT_EQ(sell.state(), OrderState::Filled);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{0});

    EXPECT_EQ(buy.state(), OrderState::PartiallyFilled);
    EXPECT_EQ(buy.remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.find(sell.id()), nullptr);
    ASSERT_NE(book.find(buy.id()), nullptr);
    EXPECT_EQ(book.find(buy.id())->remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.best_bid()->id(), buy.id());
    EXPECT_EQ(book.best_ask(), nullptr);
}

TEST(MatchingEngineTest, PartialFillRestingOrderLarger)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto sell = make_sell_order(
        1,
        Price{100},
        Quantity{150},
        10
    );

    activate(sell);
    ASSERT_TRUE(book.add(sell));

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
    EXPECT_EQ(buy.remaining_quantity(), Quantity{0});

    EXPECT_EQ(sell.state(), OrderState::PartiallyFilled);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.find(buy.id()), nullptr);

    ASSERT_NE(book.find(sell.id()), nullptr);
    EXPECT_EQ(book.find(sell.id())->remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask()->id(), sell.id());
}

TEST(MatchingEngineTest, DoesNotMatchWhenPricesDoNotCross)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto sell = make_sell_order(
        1,
        Price{105},
        Quantity{100},
        10
    );

    activate(sell);
    ASSERT_TRUE(book.add(sell));

    auto buy = make_buy_order(
        2,
        Price{101},
        Quantity{100},
        20
    );

    activate(buy);

    auto trades = engine.submit(buy);

    EXPECT_TRUE(trades.empty());

    EXPECT_EQ(sell.state(), OrderState::Active);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{100});

    EXPECT_EQ(buy.state(), OrderState::Active);
    EXPECT_EQ(buy.remaining_quantity(), Quantity{100});

    ASSERT_NE(book.find(sell.id()), nullptr);
    ASSERT_NE(book.find(buy.id()), nullptr);

    EXPECT_EQ(book.best_ask()->id(), sell.id());
    EXPECT_EQ(book.best_bid()->id(), buy.id());
}

TEST(MatchingEngineTest, MatchesAcrossMultiplePriceLevels)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto sell1 = make_sell_order(
        1,
        Price{100},
        Quantity{50},
        10
    );

    auto sell2 = make_sell_order(
        2,
        Price{101},
        Quantity{50},
        11
    );

    auto sell3 = make_sell_order(
        3,
        Price{102},
        Quantity{50},
        12
    );

    activate(sell1);
    activate(sell2);
    activate(sell3);

    ASSERT_TRUE(book.add(sell1));
    ASSERT_TRUE(book.add(sell2));
    ASSERT_TRUE(book.add(sell3));

    auto buy = make_buy_order(
        4,
        Price{105},
        Quantity{120},
        20
    );

    activate(buy);

    auto trades = engine.submit(buy);

    ASSERT_EQ(trades.size(), 3);

    EXPECT_EQ(trades[0].buy_order_id, buy.id());
    EXPECT_EQ(trades[0].sell_order_id, sell1.id());
    EXPECT_EQ(trades[0].price, Price{100});
    EXPECT_EQ(trades[0].quantity, Quantity{50});

    EXPECT_EQ(trades[1].buy_order_id, buy.id());
    EXPECT_EQ(trades[1].sell_order_id, sell2.id());
    EXPECT_EQ(trades[1].price, Price{101});
    EXPECT_EQ(trades[1].quantity, Quantity{50});

    EXPECT_EQ(trades[2].buy_order_id, buy.id());
    EXPECT_EQ(trades[2].sell_order_id, sell3.id());
    EXPECT_EQ(trades[2].price, Price{102});
    EXPECT_EQ(trades[2].quantity, Quantity{20});

    EXPECT_EQ(sell1.state(), OrderState::Filled);
    EXPECT_EQ(sell2.state(), OrderState::Filled);

    EXPECT_EQ(sell3.state(), OrderState::PartiallyFilled);
    EXPECT_EQ(sell3.remaining_quantity(), Quantity{30});

    EXPECT_EQ(buy.state(), OrderState::Filled);
    EXPECT_EQ(buy.remaining_quantity(), Quantity{0});

    EXPECT_EQ(book.find(sell1.id()), nullptr);
    EXPECT_EQ(book.find(sell2.id()), nullptr);

    ASSERT_NE(book.find(sell3.id()), nullptr);
    EXPECT_EQ(book.find(sell3.id())->remaining_quantity(), Quantity{30});

    EXPECT_EQ(book.find(buy.id()), nullptr);

    EXPECT_EQ(book.best_bid(), nullptr);
    ASSERT_NE(book.best_ask(), nullptr);
    EXPECT_EQ(book.best_ask()->id(), sell3.id());
}

TEST(MatchingEngineTest, PreservesFIFOAtSamePrice)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto sell1 = make_sell_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    auto sell2 = make_sell_order(
        2,
        Price{100},
        Quantity{100},
        11
    );

    activate(sell1);
    activate(sell2);

    ASSERT_TRUE(book.add(sell1));
    ASSERT_TRUE(book.add(sell2));

    auto buy = make_buy_order(
        3,
        Price{100},
        Quantity{150},
        20
    );

    activate(buy);

    auto trades = engine.submit(buy);

    ASSERT_EQ(trades.size(), 2);

    EXPECT_EQ(trades[0].buy_order_id, buy.id());
    EXPECT_EQ(trades[0].sell_order_id, sell1.id());
    EXPECT_EQ(trades[0].price, Price{100});
    EXPECT_EQ(trades[0].quantity, Quantity{100});

    EXPECT_EQ(trades[1].buy_order_id, buy.id());
    EXPECT_EQ(trades[1].sell_order_id, sell2.id());
    EXPECT_EQ(trades[1].price, Price{100});
    EXPECT_EQ(trades[1].quantity, Quantity{50});

    EXPECT_EQ(sell1.state(), OrderState::Filled);
    EXPECT_EQ(sell1.remaining_quantity(), Quantity{0});

    EXPECT_EQ(sell2.state(), OrderState::PartiallyFilled);
    EXPECT_EQ(sell2.remaining_quantity(), Quantity{50});

    EXPECT_EQ(buy.state(), OrderState::Filled);
    EXPECT_EQ(buy.remaining_quantity(), Quantity{0});

    EXPECT_EQ(book.find(sell1.id()), nullptr);

    ASSERT_NE(book.find(sell2.id()), nullptr);
    EXPECT_EQ(book.find(sell2.id())->remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.find(buy.id()), nullptr);

    EXPECT_EQ(book.best_bid(), nullptr);
    ASSERT_NE(book.best_ask(), nullptr);
    EXPECT_EQ(book.best_ask()->id(), sell2.id());
}

TEST(MatchingEngineTest, ExactSellBuyMatch)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto buy = make_buy_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    activate(buy);
    ASSERT_TRUE(book.add(buy));

    auto sell = make_sell_order(
        2,
        Price{99},
        Quantity{100},
        20
    );

    activate(sell);

    auto trades = engine.submit(sell);

    ASSERT_EQ(trades.size(), 1);

    const Trade& trade = trades[0];

    EXPECT_EQ(trade.buy_order_id, buy.id());
    EXPECT_EQ(trade.sell_order_id, sell.id());
    EXPECT_EQ(trade.price, Price{100});
    EXPECT_EQ(trade.quantity, Quantity{100});
    EXPECT_EQ(trade.timestamp, Timestamp{20});

    EXPECT_EQ(buy.state(), OrderState::Filled);
    EXPECT_EQ(buy.remaining_quantity(), Quantity{0});

    EXPECT_EQ(sell.state(), OrderState::Filled);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{0});

    EXPECT_EQ(book.find(buy.id()), nullptr);
    EXPECT_EQ(book.find(sell.id()), nullptr);

    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask(), nullptr);

    EXPECT_TRUE(book.empty());
}

TEST(MatchingEngineTest, PartialFillIncomingSellOrderLarger)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto buy = make_buy_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    activate(buy);
    ASSERT_TRUE(book.add(buy));

    auto sell = make_sell_order(
        2,
        Price{99},
        Quantity{150},
        20
    );

    activate(sell);

    auto trades = engine.submit(sell);

    ASSERT_EQ(trades.size(), 1);

    const Trade& trade = trades[0];

    EXPECT_EQ(trade.buy_order_id, buy.id());
    EXPECT_EQ(trade.sell_order_id, sell.id());
    EXPECT_EQ(trade.price, Price{100});
    EXPECT_EQ(trade.quantity, Quantity{100});
    EXPECT_EQ(trade.timestamp, Timestamp{20});

    EXPECT_EQ(buy.state(), OrderState::Filled);
    EXPECT_EQ(buy.remaining_quantity(), Quantity{0});

    EXPECT_EQ(sell.state(), OrderState::PartiallyFilled);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.find(buy.id()), nullptr);

    ASSERT_NE(book.find(sell.id()), nullptr);
    EXPECT_EQ(book.find(sell.id())->remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_ask()->id(), sell.id());
}

TEST(MatchingEngineTest, PartialFillRestingBuyOrderLarger)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto buy = make_buy_order(
        1,
        Price{100},
        Quantity{150},
        10
    );

    activate(buy);
    ASSERT_TRUE(book.add(buy));

    auto sell = make_sell_order(
        2,
        Price{99},
        Quantity{100},
        20
    );

    activate(sell);

    auto trades = engine.submit(sell);

    ASSERT_EQ(trades.size(), 1);

    const Trade& trade = trades[0];

    EXPECT_EQ(trade.buy_order_id, buy.id());
    EXPECT_EQ(trade.sell_order_id, sell.id());
    EXPECT_EQ(trade.price, Price{100});
    EXPECT_EQ(trade.quantity, Quantity{100});
    EXPECT_EQ(trade.timestamp, Timestamp{20});

    EXPECT_EQ(sell.state(), OrderState::Filled);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{0});

    EXPECT_EQ(buy.state(), OrderState::PartiallyFilled);
    EXPECT_EQ(buy.remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.find(sell.id()), nullptr);

    ASSERT_NE(book.find(buy.id()), nullptr);
    EXPECT_EQ(book.find(buy.id())->remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.best_ask(), nullptr);
    EXPECT_EQ(book.best_bid()->id(), buy.id());
}

TEST(MatchingEngineTest, SellMatchesAcrossMultipleBidLevels)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto buy1 = make_buy_order(
        1,
        Price{102},
        Quantity{50},
        10
    );

    auto buy2 = make_buy_order(
        2,
        Price{101},
        Quantity{50},
        11
    );

    auto buy3 = make_buy_order(
        3,
        Price{100},
        Quantity{50},
        12
    );

    activate(buy1);
    activate(buy2);
    activate(buy3);

    ASSERT_TRUE(book.add(buy1));
    ASSERT_TRUE(book.add(buy2));
    ASSERT_TRUE(book.add(buy3));

    auto sell = make_sell_order(
        4,
        Price{99},
        Quantity{120},
        20
    );

    activate(sell);

    auto trades = engine.submit(sell);

    ASSERT_EQ(trades.size(), 3);

    EXPECT_EQ(trades[0].buy_order_id, buy1.id());
    EXPECT_EQ(trades[0].sell_order_id, sell.id());
    EXPECT_EQ(trades[0].price, Price{102});
    EXPECT_EQ(trades[0].quantity, Quantity{50});

    EXPECT_EQ(trades[1].buy_order_id, buy2.id());
    EXPECT_EQ(trades[1].sell_order_id, sell.id());
    EXPECT_EQ(trades[1].price, Price{101});
    EXPECT_EQ(trades[1].quantity, Quantity{50});

    EXPECT_EQ(trades[2].buy_order_id, buy3.id());
    EXPECT_EQ(trades[2].sell_order_id, sell.id());
    EXPECT_EQ(trades[2].price, Price{100});
    EXPECT_EQ(trades[2].quantity, Quantity{20});

    EXPECT_EQ(buy1.state(), OrderState::Filled);
    EXPECT_EQ(buy2.state(), OrderState::Filled);

    EXPECT_EQ(buy3.state(), OrderState::PartiallyFilled);
    EXPECT_EQ(buy3.remaining_quantity(), Quantity{30});

    EXPECT_EQ(sell.state(), OrderState::Filled);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{0});

    EXPECT_EQ(book.find(buy1.id()), nullptr);
    EXPECT_EQ(book.find(buy2.id()), nullptr);

    ASSERT_NE(book.find(buy3.id()), nullptr);
    EXPECT_EQ(book.find(buy3.id())->remaining_quantity(), Quantity{30});

    EXPECT_EQ(book.find(sell.id()), nullptr);

    EXPECT_EQ(book.best_ask(), nullptr);
    ASSERT_NE(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_bid()->id(), buy3.id());
}

TEST(MatchingEngineTest, BuyWithNoOpposingLiquidityBecomesRestingOrder)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto buy = make_buy_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    activate(buy);

    auto trades = engine.submit(buy);

    EXPECT_TRUE(trades.empty());

    EXPECT_EQ(buy.state(), OrderState::Active);
    EXPECT_EQ(buy.remaining_quantity(), Quantity{100});

    ASSERT_NE(book.find(buy.id()), nullptr);
    EXPECT_EQ(book.find(buy.id()), &buy);

    ASSERT_NE(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_bid()->id(), buy.id());

    EXPECT_EQ(book.best_ask(), nullptr);
}

TEST(MatchingEngineTest, SellWithNoOpposingLiquidityBecomesRestingOrder)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto sell = make_sell_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    activate(sell);

    auto trades = engine.submit(sell);

    EXPECT_TRUE(trades.empty());

    EXPECT_EQ(sell.state(), OrderState::Active);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{100});

    ASSERT_NE(book.find(sell.id()), nullptr);
    EXPECT_EQ(book.find(sell.id()), &sell);

    EXPECT_EQ(book.best_bid(), nullptr);

    ASSERT_NE(book.best_ask(), nullptr);
    EXPECT_EQ(book.best_ask()->id(), sell.id());
}

TEST(MatchingEngineTest, SellDoesNotMatchWhenPriceIsAboveBestBid)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto buy = make_buy_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    activate(buy);
    ASSERT_TRUE(book.add(buy));

    auto sell = make_sell_order(
        2,
        Price{105},
        Quantity{100},
        20
    );

    activate(sell);

    auto trades = engine.submit(sell);

    EXPECT_TRUE(trades.empty());

    EXPECT_EQ(buy.state(), OrderState::Active);
    EXPECT_EQ(buy.remaining_quantity(), Quantity{100});

    EXPECT_EQ(sell.state(), OrderState::Active);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{100});

    ASSERT_NE(book.find(buy.id()), nullptr);
    ASSERT_NE(book.find(sell.id()), nullptr);

    EXPECT_EQ(book.best_bid()->id(), buy.id());
    EXPECT_EQ(book.best_ask()->id(), sell.id());
}

TEST(MatchingEngineTest, PreservesFIFOBetweenBidsAtSamePrice)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto buy1 = make_buy_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    auto buy2 = make_buy_order(
        2,
        Price{100},
        Quantity{100},
        11
    );

    activate(buy1);
    activate(buy2);

    ASSERT_TRUE(book.add(buy1));
    ASSERT_TRUE(book.add(buy2));

    auto sell = make_sell_order(
        3,
        Price{99},
        Quantity{150},
        20
    );

    activate(sell);

    auto trades = engine.submit(sell);

    ASSERT_EQ(trades.size(), 2);

    EXPECT_EQ(trades[0].buy_order_id, buy1.id());
    EXPECT_EQ(trades[0].sell_order_id, sell.id());
    EXPECT_EQ(trades[0].price, Price{100});
    EXPECT_EQ(trades[0].quantity, Quantity{100});

    EXPECT_EQ(trades[1].buy_order_id, buy2.id());
    EXPECT_EQ(trades[1].sell_order_id, sell.id());
    EXPECT_EQ(trades[1].price, Price{100});
    EXPECT_EQ(trades[1].quantity, Quantity{50});

    EXPECT_EQ(buy1.state(), OrderState::Filled);
    EXPECT_EQ(buy1.remaining_quantity(), Quantity{0});

    EXPECT_EQ(buy2.state(), OrderState::PartiallyFilled);
    EXPECT_EQ(buy2.remaining_quantity(), Quantity{50});

    EXPECT_EQ(sell.state(), OrderState::Filled);
    EXPECT_EQ(sell.remaining_quantity(), Quantity{0});

    EXPECT_EQ(book.find(buy1.id()), nullptr);

    ASSERT_NE(book.find(buy2.id()), nullptr);
    EXPECT_EQ(book.find(buy2.id())->remaining_quantity(), Quantity{50});

    EXPECT_EQ(book.find(sell.id()), nullptr);

    EXPECT_EQ(book.best_ask(), nullptr);
    ASSERT_NE(book.best_bid(), nullptr);
    EXPECT_EQ(book.best_bid()->id(), buy2.id());
}

TEST(MatchingEngineTest, RejectsNewOrder)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto buy = make_buy_order(
        1,
        Price{100},
        Quantity{100},
        10
    );

    auto trades = engine.submit(buy);

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(buy.state(), OrderState::New);
    EXPECT_TRUE(book.empty());
}

TEST(MatchingEngineTest, DoesNotTradeZeroQuantity)
{
    OrderBook book;
    MatchingEngine engine{book};

    auto buy = make_buy_order(
        1,
        Price{100},
        Quantity{0},
        10
    );

    activate(buy);

    auto trades = engine.submit(buy);

    EXPECT_TRUE(trades.empty());
    EXPECT_TRUE(book.empty());
}

} // namespace simulator