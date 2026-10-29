#include "chronos/server/session_strategy.hpp"

namespace simulator::server {

std::unique_ptr<AnyStrategy> make_strategy(const StrategySpec& spec, const StrategySetup& setup)
{
    const StrategyInfo* info = spec.name.empty() ? nullptr : find_strategy(spec.name);
    return info != nullptr ? info->make(spec.params, setup) : nullptr;
}

bool strategy_fits(const StrategySpec& spec)
{
    const StrategyInfo* info = find_strategy(spec.name);
    ParamList resolved;
    std::string bad;
    return info != nullptr && resolve_params(info->params, spec.params, resolved, bad);
}

StrategySpec strategy_spec(std::string_view name, std::span<const Param> given)
{
    const StrategyInfo* info = find_strategy(name);
    StrategySpec spec{.name = std::string{name}};
    std::string bad;
    return info != nullptr && resolve_params(info->params, given, spec.params, bad) ? spec : StrategySpec{};
}

}  // namespace simulator::server
