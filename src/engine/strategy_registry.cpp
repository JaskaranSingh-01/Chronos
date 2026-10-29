#include "simulator/engine/strategy_registry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace simulator {

namespace {

// Function-local, so registrations from other files' initializers find it built whatever their order.
std::vector<StrategyInfo>& registry()
{
    static std::vector<StrategyInfo> strategies;
    return strategies;
}

[[noreturn]] void refuse(const std::string& name, const char* why)
{
    std::fprintf(stderr, "strategy plugin '%s': %s\n", name.c_str(), why);
    std::abort();
}

bool is_integer(const ParamValue& value) noexcept
{
    return std::holds_alternative<std::int64_t>(value);
}

}  // namespace

bool register_strategy(StrategyInfo info)
{
    if (!valid_strategy_name(info.name)) {
        refuse(info.name, "a name is 1-40 of a-z, 0-9 and _");
    }
    if (find_strategy(info.name) != nullptr) {
        refuse(info.name, "registered twice: each name must be unique");
    }
    if (!info.limits || !info.make) {
        refuse(info.name, "needs limits, and make unless the strategy is built from its params");
    }
    for (const ParamInfo& param : info.params) {
        if (!valid_strategy_name(param.name) || !param_fits(param, param.default_value)) {
            refuse(info.name + "." + param.name,
                   "a param needs a 1-40 char [a-z0-9_] name and a default in [min, max]");
        }
    }
    std::vector<StrategyInfo>& strategies = registry();
    const auto at = std::lower_bound(strategies.begin(), strategies.end(), info.name,
                                     [](const StrategyInfo& s, const std::string& name) { return s.name < name; });
    strategies.insert(at, std::move(info));
    return true;
}

const std::vector<StrategyInfo>& registered_strategies()
{
    return registry();
}

const StrategyInfo* find_strategy(std::string_view name) noexcept
{
    for (const StrategyInfo& info : registry()) {
        if (info.name == name) {
            return &info;
        }
    }
    return nullptr;
}

bool valid_strategy_name(std::string_view name) noexcept
{
    return !name.empty() && name.size() <= 40 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool param_fits(const ParamInfo& param, const ParamValue& value) noexcept
{
    if (is_integer(param.default_value)) {
        if (!is_integer(value) || !is_integer(param.min) || !is_integer(param.max)) {
            return false;
        }
        const std::int64_t v = std::get<std::int64_t>(value);
        return v >= std::get<std::int64_t>(param.min) && v <= std::get<std::int64_t>(param.max);
    }
    const auto as_double = [](const ParamValue& v) {
        return is_integer(v) ? static_cast<double>(std::get<std::int64_t>(v)) : std::get<double>(v);
    };
    const double v = as_double(value);
    return std::isfinite(v) && v >= as_double(param.min) && v <= as_double(param.max);
}

bool parse_param(const ParamInfo& param, std::string_view text, ParamValue& value)
{
    const char* end = text.data() + text.size();
    if (is_integer(param.default_value)) {
        std::int64_t v{0};
        const auto [ptr, ec] = std::from_chars(text.data(), end, v);
        if (text.empty() || ec != std::errc{} || ptr != end || !param_fits(param, v)) {
            return false;
        }
        value = v;
        return true;
    }
    // strtod, not from_chars: Apple's libc++ lacks the floating-point from_chars.
    if (text.empty() || text.find_first_of(" \t\n") != std::string_view::npos) {
        return false;
    }
    const std::string copy{text};
    char* parsed_end = nullptr;
    const double v = std::strtod(copy.c_str(), &parsed_end);
    if (parsed_end != copy.c_str() + copy.size() || !param_fits(param, v)) {
        return false;
    }
    value = v;
    return true;
}

bool resolve_params(std::span<const ParamInfo> params, std::span<const Param> given, ParamList& out, std::string& bad)
{
    ParamList values;
    for (const ParamInfo& param : params) {
        values.push_back({param.name, param.default_value});
    }
    for (const Param& g : given) {
        const auto it =
            std::find_if(params.begin(), params.end(), [&](const ParamInfo& p) { return p.name == g.name; });
        if (it == params.end() || !param_fits(*it, g.value)) {
            bad = g.name;
            return false;
        }
        ParamValue value = g.value;
        if (!is_integer(it->default_value) && is_integer(value)) {
            value = static_cast<double>(std::get<std::int64_t>(value));
        }
        values[static_cast<std::size_t>(it - params.begin())].value = value;
    }
    std::sort(values.begin(), values.end(), [](const Param& a, const Param& b) { return a.name < b.name; });
    out = std::move(values);
    return true;
}

const ParamValue* find_param(const ParamList& values, std::string_view name) noexcept
{
    for (const Param& p : values) {
        if (p.name == name) {
            return &p.value;
        }
    }
    return nullptr;
}

}  // namespace simulator
