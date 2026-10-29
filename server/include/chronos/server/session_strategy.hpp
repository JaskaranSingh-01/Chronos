#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "simulator/engine/strategy_registry.hpp"
#include "simulator/sim/execution_simulator.hpp"

namespace simulator::server {

// A strategy and its params: a registered one by name, or a Lua script with its source.
struct StrategySpec
{
    std::string name{};    // "" = none: manual trading
    ParamList params{};    // every param of the strategy, sorted by name
    std::string source{};  // a script's Lua source; "" for a registered strategy

    bool operator==(const StrategySpec&) const = default;
};

// The strategy `spec` names, built for `setup`; null for none or a name not registered.
std::unique_ptr<AnyStrategy> make_strategy(const StrategySpec& spec, const StrategySetup& setup);

// Whether `spec` names a registered strategy, or a script that loads, and each of its params fits it.
bool strategy_fits(const StrategySpec& spec);

// The registered strategy `name` with its default params but for `given`; none if the name or a param is unknown or a
// value does not fit.
StrategySpec strategy_spec(std::string_view name, std::span<const Param> given = {});

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
    std::unique_ptr<AnyStrategy> active_;  // null: manual trading
    StrategySpec spec_{};
    const ExecutionSimulator& execution_;  // the resting orders a close cancels
    TradingWindow window_;
    std::vector<FeedSession> sessions_;  // each day's, oldest first
    std::vector<DaySession> day_sessions_;  // the same, as a strategy reads them
    unsigned price_scale_;
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
                    std::vector<FeedSession> sessions, unsigned price_scale)
        : execution_(execution), window_(window), sessions_(std::move(sessions)), price_scale_(price_scale)
    {
        delivered.reserve(4'096);
        for (const FeedSession& day : sessions_) {
            day_sessions_.push_back(DaySession{.open = day.open, .close = day.close});
        }
        attach(spec);
    }

    void attach(const StrategySpec& spec)
    {
        spec_ = spec;
        active_ = make_strategy(spec, {.sessions = day_sessions_, .price_scale = price_scale_});
    }

    const StrategySpec& spec() const noexcept
    {
        return spec_;
    }

    // The attached strategy's own counters; none without one.
    std::vector<Stat> stats() const
    {
        std::vector<Stat> out;
        if (active_ != nullptr) {
            active_->stats(out);
        }
        return out;
    }

    // Why the attached strategy stopped (a script's error), or empty.
    std::string_view error() const noexcept
    {
        return active_ != nullptr ? active_->error() : std::string_view{};
    }

    // The lines the attached strategy logged.
    std::span<const std::string> log() const noexcept
    {
        return active_ != nullptr ? active_->log() : std::span<const std::string>{};
    }

    bool attached() const noexcept
    {
        return active_ != nullptr;
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
        if (active_ != nullptr) {
            active_->on_market(ctx, event);
        }
    }

    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept
    {
        delivered.push_back(report);
        // An order that arrived before the close rested at the timer, which cancelled it.
        if (windowed() && !open_ && report.type == ReportType::Accepted && report.exchange_time >= close_) {
            ctx.gateway.cancel(report.order_id);  // it was in flight at the close
        }
        if (active_ != nullptr) {
            active_->on_report(ctx, report);
        }
    }

    // The close timer shuts the gateway and cancels every resting order.
    // ponytail: an order whose own cancel is in flight is cancelled again, so a CancelRejected is possible; track
    // pending cancels to avoid it.
    void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept
    {
        if (timer_id != close_timer) {
            if (active_ != nullptr) {
                active_->on_timer(ctx, timer_id);
            }
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
