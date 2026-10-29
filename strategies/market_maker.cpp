// The market maker as a plugin: include/simulator/strategies/market_maker.hpp holds the strategy itself.
#include "simulator/strategies/market_maker.hpp"
#include "simulator/engine/strategy_registry.hpp"

namespace {

using namespace simulator;

constexpr std::uint64_t max_size = 1'000'000'000;  // keeps sizes x prices far from int64 overflow
constexpr Timestamp max_cooldown_ns = 1'000'000'000'000'000;

[[maybe_unused]] const bool registered = register_plugin(StrategyPlugin<MarketMaker, MarketMakerParams>{
    .name = "market_maker",
    .label = "Market maker",
    .summary = "A toy: post-only quotes joining the best bid and ask, one per side",
    .params = {param("quote_size", &MarketMakerParams::quote_size, 1, max_size, "shares per quote"),
               param("max_position", &MarketMakerParams::max_position, 0, max_size,
                     "|position| a fill of the next quote may not exceed"),
               param("requote_cooldown_ns", &MarketMakerParams::requote_cooldown_ns, 0, max_cooldown_ns,
                     "the youngest quote that may be moved, in ns"),
               param("improve_ticks", &MarketMakerParams::improve_ticks, 0, max_size,
                     "grid ticks inside the touch, while the spread is over twice it")},
    .limits =
        [](const MarketMakerParams& p) {
            return RiskLimits{
                .max_position = p.max_position, .max_order_quantity = Quantity{p.quote_size}, .max_open_orders = 2};
        },
    .stats =
        [](const MarketMaker& s, std::vector<Stat>& out) {
            const MarketMakerStats& m = s.stats();
            out = {{"Quotes sent", static_cast<double>(m.quotes)},
                   {"Risk rejects", static_cast<double>(m.risk_rejects)},
                   {"Cancels sent", static_cast<double>(m.cancels)},
                   {"Fills", static_cast<double>(m.fills)},
                   {"Rejects (would cross or not trading)", static_cast<double>(m.rejects)},
                   {"Bought", static_cast<double>(m.bought)},
                   {"Sold", static_cast<double>(m.sold)}};
        },
});

}  // namespace
