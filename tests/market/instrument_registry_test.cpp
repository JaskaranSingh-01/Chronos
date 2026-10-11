#include <gtest/gtest.h>

#include "simulator/market/instrument_registry.hpp"

namespace simulator {
namespace {

const Instrument aapl{.venue = {"XNAS"}, .native_symbol = {"AAPL"}, .price_scale = 4};
const Instrument msft{.venue = {"XNAS"}, .native_symbol = {"MSFT"}, .price_scale = 4};
const Instrument btc{
    .venue = {"CBSE"}, .native_symbol = {"BTC-USD"},
    .price_scale = 2, .quantity_scale = 8, .tick_size = 1};

TEST(InstrumentRegistryTest, AssignsDenseIdsFromZero)
{
    InstrumentRegistry registry{4};
    SymbolId id{99};

    EXPECT_EQ(registry.add(aapl, id), RegisterResult::Added);
    EXPECT_EQ(id, SymbolId{0});
    EXPECT_EQ(registry.add(msft, id), RegisterResult::Added);
    EXPECT_EQ(id, SymbolId{1});
    EXPECT_EQ(registry.add(btc, id), RegisterResult::Added);
    EXPECT_EQ(id, SymbolId{2});
    EXPECT_EQ(registry.size(), 3u);

    ASSERT_NE(registry.find(1), nullptr);
    EXPECT_EQ(*registry.find(1), msft);
}

TEST(InstrumentRegistryTest, SameInstrumentTwiceIsExistingWithTheSameId)
{
    InstrumentRegistry registry{4};
    SymbolId first{99};
    SymbolId second{99};

    ASSERT_EQ(registry.add(msft, first), RegisterResult::Added);
    ASSERT_EQ(registry.add(aapl, first), RegisterResult::Added);
    EXPECT_EQ(registry.add(aapl, second), RegisterResult::Existing);
    EXPECT_EQ(second, first);
    EXPECT_EQ(registry.size(), 2u);
}

TEST(InstrumentRegistryTest, SameKeyWithDifferentScalesIsConflict)
{
    InstrumentRegistry registry{4};
    SymbolId id{99};
    ASSERT_EQ(registry.add(aapl, id), RegisterResult::Added);

    Instrument other_scale = aapl;
    other_scale.price_scale = 2;
    Instrument other_tick = aapl;
    other_tick.tick_size = 100;

    id = 99;
    EXPECT_EQ(registry.add(other_scale, id), RegisterResult::Conflict);
    EXPECT_EQ(registry.add(other_tick, id), RegisterResult::Conflict);
    EXPECT_EQ(id, SymbolId{99});
    EXPECT_EQ(registry.size(), 1u);
}

TEST(InstrumentRegistryTest, SameSymbolOnAnotherVenueIsANewInstrument)
{
    InstrumentRegistry registry{4};
    SymbolId id{99};
    ASSERT_EQ(registry.add(aapl, id), RegisterResult::Added);

    Instrument other_venue = aapl;
    other_venue.venue = {"ARCX"};

    EXPECT_EQ(registry.add(other_venue, id), RegisterResult::Added);
    EXPECT_EQ(id, SymbolId{1});
}

TEST(InstrumentRegistryTest, FullAtCapacity)
{
    InstrumentRegistry registry{2};
    SymbolId id{99};
    ASSERT_EQ(registry.add(aapl, id), RegisterResult::Added);
    ASSERT_EQ(registry.add(msft, id), RegisterResult::Added);

    id = 99;
    EXPECT_EQ(registry.add(btc, id), RegisterResult::Full);
    EXPECT_EQ(id, SymbolId{99});
    EXPECT_EQ(registry.size(), 2u);

    // A known instrument is still found when full.
    EXPECT_EQ(registry.add(aapl, id), RegisterResult::Existing);
    EXPECT_EQ(id, SymbolId{0});
}

TEST(InstrumentRegistryTest, ZeroCapacityIsAlwaysFull)
{
    InstrumentRegistry registry{0};
    SymbolId id{99};

    EXPECT_EQ(registry.add(aapl, id), RegisterResult::Full);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(InstrumentRegistryTest, FindOfUnknownIdIsNull)
{
    InstrumentRegistry registry{4};
    SymbolId id{99};

    EXPECT_EQ(registry.find(0), nullptr);
    ASSERT_EQ(registry.add(aapl, id), RegisterResult::Added);
    EXPECT_NE(registry.find(0), nullptr);
    EXPECT_EQ(registry.find(1), nullptr);
}

} // namespace
} // namespace simulator
