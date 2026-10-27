#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

#include "simulator/engine/strategy.hpp"
#include "simulator/sim/execution_simulator.hpp"
#include "simulator/strategies/imbalance_taker.hpp"
#include "simulator/strategies/market_maker.hpp"
#include "simulator/strategies/orb.hpp"

namespace simulator::server {

enum class SessionStrategyKind : std::uint8_t {
    None,  // manual trading
    MarketMaker,
    ImbalanceTaker,
    Orb
};

struct StrategySpec
{
    SessionStrategyKind kind{SessionStrategyKind::None};
    MarketMakerParams market_maker{};
    ImbalanceTakerParams imbalance{};
    OrbParams orb{};

    bool operator==(const StrategySpec&) const = default;
};

enum class TradingWindow : std::uint8_t {
    Regular,  // each day's feed session: a run's default
    FullDay
};

enum class SessionSource : std::uint8_t {
    Feed,     // the day's System Events 'Q' (start of market hours) and 'M' (end of market hours)
    Fallback  // either is missing: 09:30-16:00 ET
};

// A day's regular hours, [open, close) in epoch ns.
struct FeedSession
{
    Timestamp open{0};
    Timestamp close{0};
    SessionSource source{SessionSource::Fallback};

    bool operator==(const FeedSession&) const = default;
};

// Queues a cancel of every resting order; the gateway only queues, so the span stays valid.
inline void cancel_resting(const ExecutionSimulator& execution, OrderGateway& gateway) noexcept
{
    for (const ShadowOrder& order : execution.resting_orders()) {
        gateway.cancel(order.id);
    }
}

// The strategy a session's EventLoop drives, swapped at command boundaries; keeps every report delivered to it. Under a
// Regular window an attached strategy trades only inside each day's feed session.
class SessionStrategy
{
private:
    std::variant<NullStrategy, MarketMaker, ImbalanceTaker, Orb> active_;
    StrategySpec spec_{};
    const ExecutionSimulator& execution_;  // the resting orders a close cancels
    TradingWindow window_;
    std::vector<FeedSession> sessions_;  // each day's, oldest first
    std::vector<OrbSession> orb_sessions_;  // the same, as an Orb reads them
    std::size_t day_{0};  // each Clear starts the next
    bool open_{false};  // from the first market event inside the day's session to its close timer
    Timestamp close_{0};  // the close of the window opened last

    bool windowed() const noexcept
    {
        return window_ == TradingWindow::Regular && attached();
    }

public:
    // The timer id of a day's close; the strategies use none.
    static constexpr std::uint64_t close_timer = std::numeric_limits<std::uint64_t>::max();

    std::vector<ExecutionReport> delivered;  // the session drains it after each loop call

    SessionStrategy(const StrategySpec& spec, const ExecutionSimulator& execution, TradingWindow window,
                    std::vector<FeedSession> sessions)
        : execution_(execution), window_(window), sessions_(std::move(sessions))
    {
        delivered.reserve(4'096);
        for (const FeedSession& day : sessions_) {
            orb_sessions_.push_back(OrbSession{.open = day.open, .close = day.close});
        }
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
        case SessionStrategyKind::Orb:
            active_.emplace<Orb>(spec.orb, orb_sessions_);
            return;
        }
    }

    const StrategySpec& spec() const noexcept
    {
        return spec_;
    }

    // The attached ORB's stats, or null.
    const OrbStats* orb_stats() const noexcept
    {
        const Orb* orb = std::get_if<Orb>(&active_);
        return orb != nullptr ? &orb->stats() : nullptr;
    }

    bool attached() const noexcept
    {
        return !std::holds_alternative<NullStrategy>(active_);
    }

    const std::vector<FeedSession>& sessions() const noexcept
    {
        return sessions_;
    }

    // Whether the gateway takes orders: always, but for a windowed strategy outside its window.
    bool gateway_open() const noexcept
    {
        return !windowed() || open_;
    }

    // A windowed strategy sees no market event outside the day's session; the first one inside opens its window.
    void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept
    {
        if (event.type == MarketEventType::Clear) {
            ++day_;
        }
        if (windowed()) {
            if (day_ >= sessions_.size() || event.timestamp < sessions_[day_].open ||
                event.timestamp >= sessions_[day_].close) {
                return;
            }
            if (!open_) {
                open_ = true;
                close_ = sessions_[day_].close;
                ctx.gateway.set_open(true);
                ctx.set_timer(close_, close_timer);
            }
        }
        std::visit([&](auto& strategy) { strategy.on_market(ctx, event); }, active_);
    }

    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept
    {
        delivered.push_back(report);
        // An order that arrived before the close rested at the timer, which cancelled it.
        if (windowed() && !open_ && report.type == ReportType::Accepted && report.exchange_time >= close_) {
            ctx.gateway.cancel(report.order_id);  // it was in flight at the close
        }
        std::visit([&](auto& strategy) { strategy.on_report(ctx, report); }, active_);
    }

    // The close timer shuts the gateway and cancels every resting order.
    // ponytail: an order whose own cancel is in flight is cancelled again, so a CancelRejected is possible; track
    // pending cancels to avoid it.
    void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept
    {
        if (timer_id != close_timer) {
            std::visit([&](auto& strategy) { strategy.on_timer(ctx, timer_id); }, active_);
            return;
        }
        open_ = false;
        ctx.gateway.set_open(gateway_open());
        if (windowed()) {
            cancel_resting(execution_, ctx.gateway);
        }
    }
};

static_assert(Strategy<SessionStrategy>);

}  // namespace simulator::server
