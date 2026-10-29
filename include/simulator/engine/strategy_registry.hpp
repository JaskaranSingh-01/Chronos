#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "simulator/engine/strategy.hpp"

namespace simulator {

// A param's value: an int64 for an integer param, a double for a real one.
using ParamValue = std::variant<std::int64_t, double>;

// One param a strategy declares; default_value's type is the param's. A real param's max may be +inf: no cap.
struct ParamInfo
{
    std::string name{};  // as --param and the API spell it, e.g. "quote_size"
    ParamValue default_value{};
    ParamValue min{};
    ParamValue max{};
    std::string help{};

    bool operator==(const ParamInfo&) const = default;
};

struct Param
{
    std::string name{};
    ParamValue value{};

    bool operator==(const Param&) const = default;
};

// Every param of a strategy, sorted by name.
using ParamList = std::vector<Param>;

// One of a strategy's own counters, shown after a run; a price is in ticks.
struct Stat
{
    std::string name{};
    double value{0};
    bool price{false};

    bool operator==(const Stat&) const = default;
};

// What a strategy is built with besides its params.
struct StrategySetup
{
    std::span<const DaySession> sessions{};  // each day's regular session, oldest first
    unsigned price_scale{4};                 // a price is in units of 10^-price_scale dollars
};

// A strategy behind the registry: the Strategy callbacks made virtual, and its counters.
class AnyStrategy
{
public:
    AnyStrategy() = default;
    AnyStrategy(const AnyStrategy&) = delete;
    AnyStrategy& operator=(const AnyStrategy&) = delete;
    virtual ~AnyStrategy() = default;

    virtual void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept = 0;
    virtual void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept = 0;
    virtual void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept = 0;
    // Appends its counters, if it keeps any.
    virtual void stats(std::vector<Stat>& out) const
    {
        (void)out;
    }
    // Why it stopped (a script's error), or empty; a stopped strategy does nothing more.
    virtual std::string_view error() const noexcept
    {
        return {};
    }
    // The lines it logged (a script's log()), kept for the run's results.
    virtual std::span<const std::string> log() const noexcept
    {
        return {};
    }
};

static_assert(Strategy<AnyStrategy>);

// A registered strategy, as the CLI, the server and the Lab see it.
struct StrategyInfo
{
    std::string name{};  // 1-40 of [a-z0-9_], as --strategy and the API spell it
    std::string label{};  // as the Lab shows it
    std::string summary{};
    std::vector<ParamInfo> params{};  // in the order a form shows them
    std::function<RiskLimits(const ParamList&)> limits{};  // the risk limits a run takes unless told otherwise
    // The param that makes a run pointless under `limits` (an order size over the position limit), or "".
    std::function<std::string(const ParamList&, const RiskLimits&)> check{};
    std::function<std::unique_ptr<AnyStrategy>(const ParamList&, const StrategySetup&)> make{};
};

// Adds `info` to the registry, at start-up; a bad or taken name, or a missing limits or make, aborts the program.
bool register_strategy(StrategyInfo info);

// Every registered strategy, sorted by name.
const std::vector<StrategyInfo>& registered_strategies();

// The registered strategy called `name`, or null.
const StrategyInfo* find_strategy(std::string_view name) noexcept;

// Whether `name` is 1-40 of [a-z0-9_].
bool valid_strategy_name(std::string_view name) noexcept;

// Whether `value` has `param`'s type (an integer for an integer param) and lies in its [min, max].
bool param_fits(const ParamInfo& param, const ParamValue& value) noexcept;

// `text` as `param`'s value: a base-10 integer, or for a real param any finite decimal. False if it is neither or
// does not fit.
bool parse_param(const ParamInfo& param, std::string_view text, ParamValue& value);

// Every one of `params`: the value `given` names, else its default, sorted by name. False, with `bad` naming it, at
// the first of `given` that is unknown or does not fit; a real param also takes an integer.
bool resolve_params(std::span<const ParamInfo> params, std::span<const Param> given, ParamList& out, std::string& bad);

// The value of the param `name` in `values`, or null.
const ParamValue* find_param(const ParamList& values, std::string_view name) noexcept;

// --- Typed registration: a plugin's strategy class and params struct --------------------------------------------

// A Strategy as an AnyStrategy, with the plugin's way to read its counters.
template <Strategy S>
class StrategyModel final : public AnyStrategy
{
private:
    S strategy_;
    void (*stats_)(const S&, std::vector<Stat>&);

public:
    template <class Make>
    StrategyModel(Make&& make, void (*stats)(const S&, std::vector<Stat>&))
        : strategy_(make()), stats_(stats)
    {}

    void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept override
    {
        strategy_.on_market(ctx, event);
    }
    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept override
    {
        strategy_.on_report(ctx, report);
    }
    void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept override
    {
        strategy_.on_timer(ctx, timer_id);
    }
    void stats(std::vector<Stat>& out) const override
    {
        if (stats_ != nullptr) {
            stats_(strategy_, out);
        }
    }

    const S& strategy() const noexcept
    {
        return strategy_;
    }
};

// One field of a plugin's params struct P.
template <class P>
struct PluginParam
{
    std::string_view name{};
    ParamValue min{};
    ParamValue max{};
    std::string_view help{};
    std::function<ParamValue(const P&)> get{};
    std::function<void(P&, const ParamValue&)> set{};
};

// Declares P::*member as a param, its bounds in its own type: param("clip", &MyParams::clip, 1, 1'000'000).
template <class P, class T>
PluginParam<P> param(std::string_view name, T P::* member, std::type_identity_t<T> min, std::type_identity_t<T> max,
                     std::string_view help = {})
{
    static_assert(std::is_arithmetic_v<T>, "a param is an integer or a floating-point field");
    const auto value = [](T v) -> ParamValue {
        if constexpr (std::is_floating_point_v<T>) {
            return static_cast<double>(v);
        } else {
            return static_cast<std::int64_t>(v);
        }
    };
    return {name,
            value(min),
            value(max),
            help,
            [member, value](const P& p) { return value(p.*member); },
            [member](P& p, const ParamValue& v) {
                if constexpr (std::is_floating_point_v<T>) {
                    p.*member = static_cast<T>(std::get<double>(v));
                } else {
                    p.*member = static_cast<T>(std::get<std::int64_t>(v));
                }
            }};
}

// A plugin: a Strategy S configured by a params struct P, whose default member values are the params' defaults.
template <class S, class P>
struct StrategyPlugin
{
    std::string_view name{};
    std::string_view label{};
    std::string_view summary{};
    std::vector<PluginParam<P>> params{};
    RiskLimits (*limits)(const P&) = nullptr;  // required: the gateway checks every order against them
    // Builds it; unset, S(params, setup) if S takes both, else S(params).
    S (*make)(const P&, const StrategySetup&) = nullptr;
    void (*stats)(const S&, std::vector<Stat>&) = nullptr;
    std::string_view (*check)(const P&, const RiskLimits&) = nullptr;
};

// Registers `plugin`; call it once, from a namespace-scope initializer in its strategies/*.cpp file.
template <Strategy S, class P>
bool register_plugin(StrategyPlugin<S, P> plugin)
{
    const auto fields = std::make_shared<const std::vector<PluginParam<P>>>(std::move(plugin.params));
    const auto typed = [fields](const ParamList& values) {
        P p{};
        for (const PluginParam<P>& field : *fields) {
            if (const ParamValue* value = find_param(values, field.name)) {
                field.set(p, *value);
            }
        }
        return p;
    };
    StrategyInfo info{
        .name = std::string{plugin.name}, .label = std::string{plugin.label}, .summary = std::string{plugin.summary}};
    const P defaults{};
    for (const PluginParam<P>& field : *fields) {
        info.params.push_back(
            {std::string{field.name}, field.get(defaults), field.min, field.max, std::string{field.help}});
    }
    if (plugin.limits != nullptr) {
        info.limits = [typed, limits = plugin.limits](const ParamList& values) { return limits(typed(values)); };
    }
    if (plugin.check != nullptr) {
        info.check = [typed, check = plugin.check](const ParamList& values, const RiskLimits& limits) {
            return std::string{check(typed(values), limits)};
        };
    }
    constexpr bool self_made =
        std::is_constructible_v<S, const P&, const StrategySetup&> || std::is_constructible_v<S, const P&>;
    if (plugin.make != nullptr || self_made) {
        info.make = [typed, make = plugin.make, stats = plugin.stats](
                        const ParamList& values, const StrategySetup& setup) -> std::unique_ptr<AnyStrategy> {
            const P p = typed(values);
            if (make != nullptr) {
                return std::make_unique<StrategyModel<S>>([&] { return make(p, setup); }, stats);
            }
            if constexpr (std::is_constructible_v<S, const P&, const StrategySetup&>) {
                return std::make_unique<StrategyModel<S>>([&] { return S(p, setup); }, stats);
            } else if constexpr (std::is_constructible_v<S, const P&>) {
                return std::make_unique<StrategyModel<S>>([&] { return S(p); }, stats);
            } else {
                return nullptr;
            }
        };
    }
    return register_strategy(std::move(info));
}

}  // namespace simulator
