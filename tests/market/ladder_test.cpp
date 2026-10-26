#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_replay.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 4;
constexpr std::int64_t kWindow = ladder_window;  // 256: the first price P's window is [P - 128, P + 128)
constexpr std::int64_t kP = 1'000'000;

std::vector<LevelInfo> levels(const MarketBook& book, Side side)
{
    std::vector<LevelInfo> out;
    book.visit_levels(side, [&](const LevelInfo& level) {
        out.push_back(level);
        return true;
    });
    return out;
}

// A list book and a ladder book given the same events: after each, the same result, the same levels and orders (the
// digest), and every invariant on both, the ladder's windows included (spec §2.3's transition tests).
class LadderTest : public testing::Test
{
protected:
    MarketBook list{kSymbol, 1'024, BookBackend::List};
    MarketBook ladder{kSymbol, 1'024, BookBackend::Ladder};
    OrderId next_id{1};

    void apply(const MarketEvent& event)
    {
        ASSERT_EQ(ladder.apply(event), list.apply(event));
        EXPECT_EQ(levels(ladder, Side::Buy), levels(list, Side::Buy));
        EXPECT_EQ(levels(ladder, Side::Sell), levels(list, Side::Sell));
        EXPECT_EQ(ladder.digest(), list.digest());
        for (const MarketBook* book : {&list, &ladder}) {
            InvariantError error{InvariantError::None};
            EXPECT_TRUE(book->check_invariants(error)) << to_string(error);
        }
    }

    OrderId add(Side side, std::int64_t price)
    {
        const OrderId id = next_id++;
        apply(MarketEvent{.order_id = id,
                          .price = Price{price},
                          .quantity = Quantity{10},
                          .symbol = kSymbol,
                          .type = MarketEventType::Add,
                          .side = side});
        return id;
    }

    void erase(OrderId id)
    {
        apply(MarketEvent{.order_id = id, .symbol = kSymbol, .type = MarketEventType::Delete});
    }

    OrderId replace(OrderId id, std::int64_t price)
    {
        const OrderId new_id = next_id++;
        apply(MarketEvent{.order_id = id,
                          .new_order_id = new_id,
                          .price = Price{price},
                          .quantity = Quantity{10},
                          .symbol = kSymbol,
                          .type = MarketEventType::Replace});
        return new_id;
    }
};

// The price `offset` ticks from P towards the best: up for bids, down for asks.
std::int64_t at(Side side, std::int64_t offset)
{
    return side == Side::Buy ? kP + offset : kP - offset;
}

TEST_F(LadderTest, InsertsAtJustInsideAndJustOutsideEachWindowEdgeOnBothSides)
{
    for (const Side side : {Side::Buy, Side::Sell}) {
        // Away from the best: the far edge stays put while the best does.
        const OrderId first = add(side, kP);
        for (const std::int64_t offset : {-kWindow / 2, -kWindow / 2 + 1, -kWindow / 2 - 1}) {
            const OrderId edge = add(side, at(side, offset));
            add(side, at(side, offset));  // the level again, found by its slot or the index
            erase(edge);
        }
        erase(first);
        // Toward the best: each becomes the best, and the window follows it.
        for (const std::int64_t offset : {kWindow / 2 - 1, kWindow / 2 - 2, kWindow / 2}) {
            const OrderId anchor = add(side, kP);
            const OrderId edge = add(side, at(side, offset));
            add(side, at(side, offset - 1));
            erase(edge);
            erase(anchor);
        }
    }
}

TEST_F(LadderTest, TheBestCrossesTheRecentringThresholdEachWayAndBack)
{
    for (const Side side : {Side::Buy, Side::Sell}) {
        // The middle half of P's window is [P - 64, P + 64).
        const OrderId p = add(side, kP);
        const OrderId inside = add(side, at(side, kWindow / 4 - 1));  // the best, still central
        const OrderId over = add(side, at(side, kWindow / 4));        // off-centre: the window recentres on it
        erase(over);                                                  // back to `inside`, central in the new window
        erase(inside);
        add(side, at(side, -1));
        add(side, at(side, -kWindow / 4 - 1));
        erase(p);  // the best falls to P - 1, off-centre in the window around P + 64: it recentres down
        add(side, at(side, kWindow / 4));
        add(side, at(side, -kWindow / 4));
    }
}

TEST_F(LadderTest, ABestFarBeyondTheWindowAndASparseBook)
{
    for (const Side side : {Side::Buy, Side::Sell}) {
        add(side, kP);
        const OrderId far = add(side, at(side, 100 * kWindow));  // a jump many windows away
        add(side, at(side, -10 * kWindow));
        add(side, at(side, -20 * kWindow));
        add(side, at(side, 99 * kWindow));
        erase(far);  // the best jumps back a window away
        add(side, at(side, -15 * kWindow));
        add(side, at(side, 50 * kWindow));
    }
}

TEST_F(LadderTest, ALevelDroppedAsTheWindowRecentresAndARecycledNodeAtANewPrice)
{
    for (const Side side : {Side::Buy, Side::Sell}) {
        const OrderId best = add(side, kP);
        add(side, at(side, -100));  // slot 28, off-centre once it is the best
        add(side, at(side, -120));
        erase(best);  // its level leaves, and the window recentres on P - 100
        // The pool hands out the freed node again: inside the new window, then outside it.
        add(side, at(side, -150));
        const OrderId outside = add(side, at(side, -100 - kWindow));
        add(side, at(side, -100 - kWindow - 1));
        erase(outside);
        add(side, at(side, -90));
    }
}

TEST_F(LadderTest, ASideEmptiedAndRefilledAtANewPrice)
{
    for (const Side side : {Side::Buy, Side::Sell}) {
        const OrderId only = add(side, kP);
        erase(only);
        const OrderId refill = add(side, at(side, 5'000));  // its window is placed on it
        add(side, at(side, 5'000 - 1));
        add(side, at(side, 5'000 - kWindow / 2));
        const OrderId replaced = replace(refill, at(side, 4'000));
        erase(replaced);
    }
}

// A stock under a cent: its window runs below zero ticks.
TEST_F(LadderTest, ATouchNearZeroTicks)
{
    for (const Side side : {Side::Buy, Side::Sell}) {
        const OrderId one = add(side, 1);
        add(side, 3);
        add(side, 200);
        add(side, 2);
        erase(one);
        add(side, 1);
        add(side, kWindow + 5);
    }
}

} // namespace
} // namespace simulator
