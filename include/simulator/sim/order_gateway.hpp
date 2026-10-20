#pragma once

#include <cstdint>

#include "simulator/portfolio/portfolio.hpp"
#include "simulator/sim/execution_report.hpp"
#include "simulator/sim/sim_event.hpp"

namespace simulator {

// The strategy's only way to act: every call queues a SimEvent.
class OrderGateway
{
private:
    SimQueue& queue_;
    Portfolio& portfolio_;
    LatencyConfig latency_;
    Timestamp now_{0};
    OrderId next_id_{1};
    bool timers_open_{true};
    bool open_{true};

public:
    OrderGateway(SimQueue& queue, Portfolio& portfolio, LatencyConfig latency) noexcept
        : queue_(queue), portfolio_(portfolio), latency_(latency)
    {}

    // Closed while the gateway is closed; else runs Portfolio::reserve, and if Accepted
    // sets `id` and queues OrderArrive at now + order_latency.
    // Otherwise nothing is reserved or queued and `id` is untouched.
    RiskCheck send(Side side, Price price, Quantity quantity, TimeInForce tif, OrderId& id) noexcept
    {
        if (!open_) {
            return RiskCheck::Closed;
        }
        const RiskCheck check = portfolio_.reserve(side, quantity);
        if (check != RiskCheck::Accepted) {
            return check;
        }
        id = next_id_++;
        queue_.push(SimEvent{
            .timestamp = now_ + latency_.order_latency,
            .type = SimEventType::OrderArrive,
            .order = OrderRequest{.id = id, .side = side, .price = price, .quantity = quantity, .time_in_force = tif}});
        return check;
    }

    // Queues CancelArrive at now + order_latency, behind any order sent before it.
    void cancel(OrderId id) noexcept
    {
        queue_.push(
            SimEvent{.timestamp = now_ + latency_.order_latency, .type = SimEventType::CancelArrive, .order_id = id});
    }

    // Schedules Strategy::on_timer(timer_id) at `at`.
    // Returns false, queuing nothing, if `at` < now() or after stop_timers().
    bool set_timer(Timestamp at, std::uint64_t timer_id) noexcept
    {
        if (at < now_ || !timers_open_) {
            return false;
        }
        queue_.push(SimEvent{.timestamp = at, .type = SimEventType::Timer, .timer_id = timer_id});
        return true;
    }

    Timestamp now() const noexcept
    {
        return now_;
    }
    void set_now(Timestamp now) noexcept
    {
        now_ = now;
    }

    // A closed gateway refuses every send(); cancel() and timers are unaffected.
    void set_open(bool open) noexcept
    {
        open_ = open;
    }

    // Makes every later set_timer fail; called at the end of the market stream.
    void stop_timers() noexcept
    {
        timers_open_ = false;
    }
};

} // namespace simulator
