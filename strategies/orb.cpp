// The opening range breakout as a plugin: include/simulator/strategies/orb.hpp holds the strategy itself.
#include "simulator/strategies/orb.hpp"
#include "simulator/engine/strategy_registry.hpp"

namespace {

using namespace simulator;

constexpr std::uint64_t max_size = 1'000'000'000;  // keeps sizes x prices far from int64 overflow

[[maybe_unused]] const bool registered = register_plugin(StrategyPlugin<Orb, OrbParams>{
    .name = "orb",
    .label = "ORB",
    .summary = "Opening range breakout: an IOC on the first trade beyond the day's opening range; a stop, and a "
               "flatten 5 minutes before the close",
    .params = {param("range_minutes", &OrbParams::range_minutes, 1, 390, "the opening range from the open, minutes"),
               param("clip", &OrbParams::clip, 1, max_size, "shares per entry")},
    .limits =
        [](const OrbParams& p) {
            return RiskLimits{.max_position = 500, .max_order_quantity = Quantity{p.clip}, .max_open_orders = 1};
        },
    .make = [](const OrbParams& p, const StrategySetup& setup) { return Orb{p, setup.sessions}; },
    .stats =
        [](const Orb& s, std::vector<Stat>& out) {
            const OrbStats& o = s.stats();
            out = {{"Long entries", static_cast<double>(o.long_entries)},
                   {"Short entries", static_cast<double>(o.short_entries)},
                   {"Stops", static_cast<double>(o.stops)},
                   {"Flattens", static_cast<double>(o.flattens)},
                   {"Closing orders", static_cast<double>(o.closing_orders)},
                   {"Risk rejects", static_cast<double>(o.risk_rejects)},
                   {"No-range days", static_cast<double>(o.no_range_days)},
                   {"Shares requested", static_cast<double>(o.requested)},
                   {"Bought", static_cast<double>(o.bought)},
                   {"Sold", static_cast<double>(o.sold)},
                   {"Expired", static_cast<double>(o.expired)}};
            if (o.has_range) {
                out.push_back({"Range low", static_cast<double>(o.range_low.ticks), true});
                out.push_back({"Range high", static_cast<double>(o.range_high.ticks), true});
            }
        },
    // The ORB enters `clip` shares at once: past either limit it could never enter.
    .check = [](const OrbParams& p, const RiskLimits& limits) -> std::string_view {
        return p.clip > static_cast<std::uint64_t>(limits.max_position) || p.clip > limits.max_order_quantity.value()
            ? "clip"
            : "";
    },
});

}  // namespace
