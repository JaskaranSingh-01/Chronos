#include <gtest/gtest.h>

#include <cstdint>

#include "simulator/market/market_event_source.hpp"

namespace simulator {
namespace {

// The smallest possible source: one event, then a sticky EndOfStream.
class OneEventSource final : public MarketEventSource
{
public:
    SourceResult next(MarketEvent& event) noexcept override
    {
        if (read_) {
            return SourceResult::EndOfStream;
        }
        read_ = true;
        event = MarketEvent{.timestamp = 100, .order_id = 1, .quantity = Quantity{10}};
        return SourceResult::Event;
    }

    std::uint64_t position() const noexcept override
    {
        return read_ ? 1 : 0;
    }

    SourceError error() const noexcept override
    {
        return SourceError::None;
    }

private:
    bool read_{false};
};

TEST(MarketEventSourceTest, TrivialSourceWorksThroughTheInterface)
{
    OneEventSource concrete;
    MarketEventSource& source = concrete;
    MarketEvent event;

    EXPECT_EQ(source.position(), 0u);
    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(event.order_id, OrderId{1});
    EXPECT_EQ(source.position(), 1u);
    EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
    EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
    EXPECT_EQ(source.error(), SourceError::None);
}

TEST(MarketEventSourceTest, InterfaceIsNoexcept)
{
    OneEventSource source;
    MarketEvent event;

    static_assert(noexcept(source.next(event)));
    static_assert(noexcept(source.position()));
    static_assert(noexcept(source.error()));
}

} // namespace
} // namespace simulator
