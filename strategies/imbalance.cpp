// The imbalance taker as a plugin: include/simulator/strategies/imbalance_taker.hpp holds the strategy itself.
#include <limits>

#include "simulator/engine/strategy_registry.hpp"
#include "simulator/strategies/imbalance_taker.hpp"

namespace {

using namespace simulator;

constexpr std::uint64_t max_size = 1'000'000'000;  // keeps sizes x prices far from int64 overflow
constexpr Timestamp max_cooldown_ns = 1'000'000'000'000'000;

[[maybe_unused]] const bool registered = register_plugin(StrategyPlugin<ImbalanceTaker, ImbalanceTakerParams>{
    .name = "imbalance",
    .label = "Imbalance",
    .summary = "A toy: an IOC at the touch when one top-of-book size is threshold times the other",
    .params = {param("threshold", &ImbalanceTakerParams::threshold, 1.0, std::numeric_limits<double>::infinity(),
                     "one side's top size over the other's that triggers an order"),
               param("clip", &ImbalanceTakerParams::clip, 1, max_size, "shares per order"),
               param("max_position", &ImbalanceTakerParams::max_position, 0, max_size,
                     "|position| the strategy itself will not exceed"),
               param("cooldown_ns", &ImbalanceTakerParams::cooldown_ns, 0, max_cooldown_ns,
                     "the wait after each order, in ns")},
    .limits =
        [](const ImbalanceTakerParams& p) {
            return RiskLimits{
                .max_position = p.max_position, .max_order_quantity = Quantity{p.clip}, .max_open_orders = 1};
        },
    .stats =
        [](const ImbalanceTaker& s, std::vector<Stat>& out) {
            const ImbalanceTakerStats& i = s.stats();
            out = {{"Orders sent", static_cast<double>(i.sent)}, {"Risk rejects", static_cast<double>(i.risk_rejects)},
                   {"Fills", static_cast<double>(i.fills)},      {"Expired", static_cast<double>(i.expired)},
                   {"Bought", static_cast<double>(i.bought)},    {"Sold", static_cast<double>(i.sold)}};
        },
});

}  // namespace
