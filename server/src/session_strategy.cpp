#include "chronos/server/session_strategy.hpp"

#include "chronos/server/script_strategy.hpp"

namespace simulator::server {

std::unique_ptr<AnyStrategy> make_strategy(const StrategySpec& spec, const StrategySetup& setup)
{
    if (is_script_name(spec.name)) {
        return make_script_strategy(spec.source, spec.params, setup);
    }
    const StrategyInfo* info = spec.name.empty() ? nullptr : find_strategy(spec.name);
    return info != nullptr ? info->make(spec.params, setup) : nullptr;
}

bool strategy_fits(const StrategySpec& spec)
{
    ParamList resolved;
    std::string bad;
    if (is_script_name(spec.name)) {  // its params against what its source declares
        const ScriptInfo script = describe_script(spec.source);
        return script.error.empty() && resolve_params(script.params, spec.params, resolved, bad);
    }
    const StrategyInfo* info = find_strategy(spec.name);
    return info != nullptr && spec.source.empty() && resolve_params(info->params, spec.params, resolved, bad);
}

StrategySpec strategy_spec(std::string_view name, std::span<const Param> given)
{
    const StrategyInfo* info = find_strategy(name);
    StrategySpec spec{.name = std::string{name}};
    std::string bad;
    return info != nullptr && resolve_params(info->params, given, spec.params, bad) ? spec : StrategySpec{};
}

}  // namespace simulator::server
