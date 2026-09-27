#include <gtest/gtest.h>

#include "simulator/events/event.hpp"

using simulator::Event;
using simulator::EventType;
using simulator::OrderId;
using simulator::Price;
using simulator::Quantity;
using simulator::Side;
using simulator::Timestamp;

TEST(EventTest, DefaultValuesAreCorrect)
{
    Event event{};

    EXPECT_EQ(event.timestamp, Timestamp{0});
    EXPECT_EQ(event.type, EventType::AddOrder);
    EXPECT_EQ(event.order_id, OrderId{0});
    EXPECT_EQ(event.side, Side::Buy);
    EXPECT_EQ(event.price, Price{});
    EXPECT_EQ(event.quantity, Quantity{});
}

TEST(EventTest, StoresTimestamp)
{
    Event event{};
    event.timestamp = 1000;

    EXPECT_EQ(event.timestamp, Timestamp{1000});
}

TEST(EventTest, StoresEventType)
{
    Event event{};
    event.type = EventType::CancelOrder;

    EXPECT_EQ(event.type, EventType::CancelOrder);
}

TEST(EventTest, StoresOrderId)
{
    Event event{};
    event.order_id = 12345;

    EXPECT_EQ(event.order_id, OrderId{12345});
}

TEST(EventTest, StoresSide)
{
    Event event{};
    event.side = Side::Sell;

    EXPECT_EQ(event.side, Side::Sell);
}

TEST(EventTest, StoresPrice)
{
    Event event{};
    event.price = Price{10050};

    EXPECT_EQ(event.price, Price{10050});
}

TEST(EventTest, StoresQuantity)
{
    Event event{};
    event.quantity = Quantity{500};

    EXPECT_EQ(event.quantity, Quantity{500});
}

TEST(EventTest, CanConstructCompleteEvent)
{
    Event event{
        Timestamp{1000},
        EventType::AddOrder,
        OrderId{12345},
        Side::Buy,
        Price{10050},
        Quantity{500}
    };

    EXPECT_EQ(event.timestamp, Timestamp{1000});
    EXPECT_EQ(event.type, EventType::AddOrder);
    EXPECT_EQ(event.order_id, OrderId{12345});
    EXPECT_EQ(event.side, Side::Buy);
    EXPECT_EQ(event.price, Price{10050});
    EXPECT_EQ(event.quantity, Quantity{500});
}