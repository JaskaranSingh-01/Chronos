#include <gtest/gtest.h>

#include "simulator/itch/itch_normalizer.hpp"

namespace simulator {

TEST(ItchDecoderTest, NormalizesAddOrder)
{
    ItchAddOrder order;

    order.timestamp = Timestamp{100};
    order.order_id = OrderId{123};
    order.side = Side::Buy;
    order.quantity = Quantity{500};
    order.price = Price{10100};

    ItchNormalizer normalizer;

    const ReplayEvent event =
        normalizer.normalize(order);

    EXPECT_EQ(
        event.timestamp,
        Timestamp{100}
    );

    EXPECT_EQ(
        event.type,
        ReplayEventType::Add
    );

    EXPECT_EQ(
        event.order_id,
        OrderId{123}
    );

    EXPECT_EQ(
        event.side,
        Side::Buy
    );

    EXPECT_EQ(
        event.quantity,
        Quantity{500}
    );

    EXPECT_EQ(
        event.price,
        Price{10100}
    );
}


TEST(ItchNormalizerTest, NormalizesOrderExecuted)
{
    ItchOrderExecuted source;

    source.timestamp = Timestamp{123456};
    source.order_id = OrderId{42};
    source.quantity = Quantity{100};
    source.match_number = 999;

    ItchNormalizer normalizer;

    const ReplayEvent event =
        normalizer.normalize(source);

    EXPECT_EQ(event.timestamp, 123456);
    EXPECT_EQ(event.type, ReplayEventType::Execute);
    EXPECT_EQ(event.order_id, 42);
    EXPECT_EQ(event.quantity.value(), 100);
}

TEST(ItchNormalizerTest, NormalizesOrderCancel)
{
    ItchOrderCancel source;

    source.timestamp = Timestamp{200};
    source.order_id = OrderId{42};
    source.quantity = Quantity{25};

    ItchNormalizer normalizer;

    const ReplayEvent event =
        normalizer.normalize(source);

    EXPECT_EQ(event.timestamp, 200);
    EXPECT_EQ(event.type, ReplayEventType::Cancel);
    EXPECT_EQ(event.order_id, 42);
    EXPECT_EQ(event.quantity.value(), 25);
}

TEST(ItchNormalizerTest, NormalizesOrderDelete)
{
    ItchOrderDelete source;

    source.timestamp = Timestamp{300};
    source.order_id = OrderId{42};

    ItchNormalizer normalizer;

    const ReplayEvent event =
        normalizer.normalize(source);

    EXPECT_EQ(event.timestamp, 300);
    EXPECT_EQ(event.type, ReplayEventType::Delete);
    EXPECT_EQ(event.order_id, 42);
}



}