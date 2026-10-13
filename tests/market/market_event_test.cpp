#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

#include "simulator/market/market_event.hpp"

namespace simulator {
namespace {

TEST(MarketEventTest, DefaultsToZeroedAdd)
{
    const MarketEvent event;

    EXPECT_EQ(event.timestamp, Timestamp{0});
    EXPECT_EQ(event.order_id, OrderId{0});
    EXPECT_EQ(event.new_order_id, OrderId{0});
    EXPECT_EQ(event.price, Price{0});
    EXPECT_EQ(event.quantity, Quantity{0});
    EXPECT_EQ(event.symbol, SymbolId{0});
    EXPECT_EQ(event.type, MarketEventType::Add);
    EXPECT_EQ(event.side, Side::Buy);
    EXPECT_EQ(event.trading_state, TradingState::Trading);
}

TEST(MarketEventTest, SupportsDesignatedInitialisation)
{
    const MarketEvent event{
        .timestamp = 1'577'716'200'000'000'000,
        .order_id = 7,
        .new_order_id = 8,
        .price = Price{1'000'100},
        .quantity = Quantity{500},
        .symbol = 3,
        .type = MarketEventType::Replace,
    };

    EXPECT_EQ(event.timestamp, Timestamp{1'577'716'200'000'000'000});
    EXPECT_EQ(event.order_id, OrderId{7});
    EXPECT_EQ(event.new_order_id, OrderId{8});
    EXPECT_EQ(event.price, Price{1'000'100});
    EXPECT_EQ(event.quantity, Quantity{500});
    EXPECT_EQ(event.symbol, SymbolId{3});
    EXPECT_EQ(event.type, MarketEventType::Replace);
    EXPECT_EQ(event.side, Side::Buy);
}

TEST(MarketEventTest, Is48Bytes)
{
    static_assert(sizeof(MarketEvent) == 48);
    static_assert(std::is_trivially_copyable_v<MarketEvent>);
}

TEST(MarketEventTest, HasNineTypes)
{
    static_assert(std::is_same_v<std::underlying_type_t<MarketEventType>, std::uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<TradingState>, std::uint8_t>);
    static_assert(std::is_same_v<SymbolId, std::uint32_t>);

    EXPECT_EQ(market_event_type_count, 9u);
    EXPECT_EQ(static_cast<std::size_t>(MarketEventType::Status) + 1, market_event_type_count);
}

} // namespace
} // namespace simulator
