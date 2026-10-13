#include <gtest/gtest.h>

#include <span>

#include "simulator/market/vector_source.hpp"

namespace simulator {
namespace {

const MarketEvent events[] = {
    MarketEvent{.timestamp = 100, .order_id = 1, .price = Price{100}, .quantity = Quantity{10}},
    MarketEvent{.timestamp = 200, .order_id = 1, .quantity = Quantity{4}, .type = MarketEventType::Cancel},
};

TEST(VectorSourceTest, EmitsEventsInOrderWithOneBasedPosition)
{
    VectorSource source{events};
    MarketEvent event;

    EXPECT_EQ(source.position(), 0u);

    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(event.timestamp, Timestamp{100});
    EXPECT_EQ(event.type, MarketEventType::Add);
    EXPECT_EQ(source.position(), 1u);

    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(event.timestamp, Timestamp{200});
    EXPECT_EQ(event.type, MarketEventType::Cancel);
    EXPECT_EQ(event.quantity, Quantity{4});
    EXPECT_EQ(source.position(), 2u);
}

TEST(VectorSourceTest, EndOfStreamIsSticky)
{
    VectorSource source{events};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Event);
    ASSERT_EQ(source.next(event), SourceResult::Event);

    EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
    EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
    EXPECT_EQ(source.position(), 2u);
    EXPECT_EQ(source.error(), SourceError::None);
}

TEST(VectorSourceTest, EmptySpanEndsAtOnce)
{
    VectorSource source{std::span<const MarketEvent>{}};
    MarketEvent event;

    EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
    EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
    EXPECT_EQ(source.position(), 0u);
}

} // namespace
} // namespace simulator
