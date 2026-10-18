#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "simulator/engine/strategy.hpp"
#include "simulator/strategies/imbalance_taker.hpp"
#include "simulator/strategies/market_maker.hpp"

namespace simulator::server {

enum class SessionStrategyKind : std::uint8_t {
    None,  // manual trading
    MarketMaker,
    ImbalanceTaker
};

struct StrategySpec
{
    SessionStrategyKind kind{SessionStrategyKind::None};
    MarketMakerParams market_maker{};
    ImbalanceTakerParams imbalance{};

    bool operator==(const StrategySpec&) const = default;
};

// The strategy a session's EventLoop drives, swapped at command boundaries; keeps every report delivered to it.
class SessionStrategy
{
private:
    std::variant<NullStrategy, MarketMaker, ImbalanceTaker> active_;
    StrategySpec spec_{};

public:
    std::vector<ExecutionReport> delivered;  // the session drains it after each loop call

    explicit SessionStrategy(const StrategySpec& spec)
    {
        delivered.reserve(4'096);
        attach(spec);
    }

    void attach(const StrategySpec& spec) noexcept
    {
        spec_ = spec;
        switch (spec.kind) {
        case SessionStrategyKind::None:
            active_.emplace<NullStrategy>();
            return;
        case SessionStrategyKind::MarketMaker:
            active_.emplace<MarketMaker>(spec.market_maker);
            return;
        case SessionStrategyKind::ImbalanceTaker:
            active_.emplace<ImbalanceTaker>(spec.imbalance);
            return;
        }
    }

    const StrategySpec& spec() const noexcept
    {
        return spec_;
    }

    bool attached() const noexcept
    {
        return !std::holds_alternative<NullStrategy>(active_);
    }

    void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept
    {
        std::visit([&](auto& strategy) { strategy.on_market(ctx, event); }, active_);
    }

    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept
    {
        delivered.push_back(report);
        std::visit([&](auto& strategy) { strategy.on_report(ctx, report); }, active_);
    }

    void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept
    {
        std::visit([&](auto& strategy) { strategy.on_timer(ctx, timer_id); }, active_);
    }
};

static_assert(Strategy<SessionStrategy>);

}  // namespace simulator::server
