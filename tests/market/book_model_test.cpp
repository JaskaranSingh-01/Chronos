#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_replay.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 2;

// One level as a book shows it: its price, total and FIFO of (id, remaining).
struct Level
{
    std::int64_t price{0};
    std::uint64_t total{0};
    std::vector<std::pair<OrderId, std::uint64_t>> orders{};

    bool operator==(const Level&) const = default;
};

// The book as plain std::maps: what every level backend must show after every operation.
class Model
{
private:
    using Fifo = std::deque<std::pair<OrderId, std::uint64_t>>;
    std::map<std::int64_t, Fifo> sides_[2];                    // index = Side, by price
    std::map<OrderId, std::pair<Side, std::int64_t>> where_;  // live orders

    std::map<std::int64_t, Fifo>& levels_of(Side side)
    {
        return sides_[static_cast<std::size_t>(side)];
    }

    void add(Side side, std::int64_t price, OrderId id, std::uint64_t quantity)
    {
        levels_of(side)[price].emplace_back(id, quantity);
        where_[id] = {side, price};
    }

    // Takes `quantity` off order `id`; at zero it leaves its level, and an emptied level leaves the book.
    void reduce(OrderId id, std::uint64_t quantity)
    {
        const auto [side, price] = where_.at(id);
        Fifo& fifo = levels_of(side).at(price);
        auto it = fifo.begin();
        while (it->first != id) {
            ++it;
        }
        it->second -= quantity;
        if (it->second == 0) {
            fifo.erase(it);
            where_.erase(id);
            if (fifo.empty()) {
                levels_of(side).erase(price);
            }
        }
    }

public:
    std::vector<OrderId> live() const
    {
        std::vector<OrderId> ids;
        for (const auto& [id, at] : where_) {
            ids.push_back(id);
        }
        return ids;
    }

    Side side(OrderId id) const
    {
        return where_.at(id).first;
    }

    std::uint64_t remaining(OrderId id) const
    {
        const auto [side, price] = where_.at(id);
        for (const auto& [order, left] : sides_[static_cast<std::size_t>(side)].at(price)) {
            if (order == id) {
                return left;
            }
        }
        return 0;
    }

    // Valid events only, as MarketBook applies them.
    void apply(const MarketEvent& e)
    {
        switch (e.type) {
        case MarketEventType::Add:
            add(e.side, e.price.ticks, e.order_id, e.quantity.value());
            break;
        case MarketEventType::Execute:
        case MarketEventType::Cancel:
            reduce(e.order_id, e.quantity.value());
            break;
        case MarketEventType::Delete:
            reduce(e.order_id, remaining(e.order_id));
            break;
        case MarketEventType::Replace: {
            const Side old_side = side(e.order_id);
            reduce(e.order_id, remaining(e.order_id));
            add(old_side, e.price.ticks, e.new_order_id, e.quantity.value());
            break;
        }
        default:
            break;
        }
    }

    // Best first: bids from the highest price, asks from the lowest.
    std::vector<Level> levels(Side side) const
    {
        std::vector<Level> out;
        for (const auto& [price, fifo] : sides_[static_cast<std::size_t>(side)]) {
            Level level{.price = price};
            for (const auto& order : fifo) {
                level.total += order.second;
                level.orders.push_back(order);
            }
            out.push_back(level);
        }
        if (side == Side::Buy) {
            std::reverse(out.begin(), out.end());
        }
        return out;
    }
};

std::vector<Level> levels(const MarketBook& book, Side side)
{
    std::vector<Level> out;
    book.visit_levels(side, [&](const LevelInfo& info) {
        Level level{.price = info.price.ticks, .total = info.quantity};
        book.visit_orders(side, info.price, [&](const Order& order) {
            level.orders.emplace_back(order.id(), order.remaining_quantity().value());
            return true;
        });
        EXPECT_EQ(level.orders.size(), info.order_count);
        out.push_back(level);
        return true;
    });
    return out;
}

// The next of a seeded stream of valid events, or none when the mid moves instead: Adds mostly near a drifting mid,
// some far from it, and now and then a jump of the mid by thousands of ticks; Executes and Cancels, partial and full;
// Deletes; Replaces to a new price.
std::optional<MarketEvent> next(const Model& model, std::mt19937_64& rng, std::int64_t& mid, OrderId& next_id)
{
    const std::vector<OrderId> live = model.live();
    const std::uint64_t roll = rng() % 100;
    auto price = [&](Side side) {
        const std::uint64_t reach = rng() % 100;
        const std::uint64_t span = reach < 80 ? 30 : reach < 95 ? 2'000 : 20'000;  // mostly near the mid
        const auto offset = static_cast<std::int64_t>(rng() % span);
        return Price{side == Side::Buy ? mid - offset : mid + 1 + offset};
    };
    auto pick = [&] { return live[rng() % live.size()]; };

    if (roll < 3) {
        mid += rng() % 2 == 0 ? 3'000 : -3'000;
        return std::nullopt;
    }
    if (roll < 10) {
        mid += static_cast<std::int64_t>(rng() % 11) - 5;
        return std::nullopt;
    }
    if (live.size() < 8 || (roll < 50 && live.size() < 300)) {
        const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        return MarketEvent{.order_id = next_id++,
                           .price = price(side),
                           .quantity = Quantity{1 + rng() % 500},
                           .symbol = kSymbol,
                           .type = MarketEventType::Add,
                           .side = side};
    }
    const OrderId id = pick();
    if (roll < 70) {
        return MarketEvent{.order_id = id, .symbol = kSymbol, .type = MarketEventType::Delete};
    }
    if (roll < 90) {
        const std::uint64_t left = model.remaining(id);
        return MarketEvent{.order_id = id,
                           .quantity = Quantity{rng() % 2 == 0 ? left : 1 + rng() % left},
                           .symbol = kSymbol,
                           .type = rng() % 2 == 0 ? MarketEventType::Execute : MarketEventType::Cancel};
    }
    return MarketEvent{.order_id = id,
                       .new_order_id = next_id++,
                       .price = price(model.side(id)),
                       .quantity = Quantity{1 + rng() % 500},
                       .symbol = kSymbol,
                       .type = MarketEventType::Replace};
}

// Both backends against one model, over the same stream (spec §6, Differential); the ladder also at a step of 4,
// so a quarter of the prices are on its grid.
TEST(BookModelTest, ARandomStreamGivesTheModelsBookAfterEveryOperation)
{
    for (const auto& [backend, step] :
         {std::pair{BookBackend::List, std::uint64_t{1}}, std::pair{BookBackend::Ladder, std::uint64_t{1}},
          std::pair{BookBackend::Ladder, std::uint64_t{4}}}) {
        SCOPED_TRACE(backend == BookBackend::List ? "list" : "ladder, step " + std::to_string(step));
        MarketBook book{kSymbol, 4'096, backend, step};
        Model model;
        std::mt19937_64 rng{11};
        std::int64_t mid = 1'000'000;
        OrderId next_id = 1;

        for (int step = 0; step < 2'000; ++step) {
            const std::optional<MarketEvent> event = next(model, rng, mid, next_id);
            if (!event) {
                continue;
            }
            ASSERT_EQ(book.apply(*event), ApplyResult::Applied) << "step " << step;
            model.apply(*event);
            ASSERT_EQ(levels(book, Side::Buy), model.levels(Side::Buy)) << "step " << step;
            ASSERT_EQ(levels(book, Side::Sell), model.levels(Side::Sell)) << "step " << step;
            InvariantError error{InvariantError::None};
            ASSERT_TRUE(book.check_invariants(error)) << "step " << step << ": " << to_string(error);
        }
        EXPECT_GT(book.order_count(), 100u);
    }
}

} // namespace
} // namespace simulator
