#pragma once

#include <cstdint>

#include "simulator/portfolio/portfolio.hpp"
#include "simulator/sim/execution_report.hpp"
#include "simulator/sim/sim_event.hpp"

namespace simulator {

// The strategy's only way to act (spec §4.9). Everything it does becomes a
// SimEvent in the queue; the EventLoop sets now() before each callback.
// Our order ids are their own sequence, from 1, and never reach MarketBook (D1).
class OrderGateway
{
private:
    SimQueue& queue_;
    Portfolio& portfolio_;
    LatencyConfig latency_;
    Timestamp now_{0};
    OrderId next_id_{1};

public:
    OrderGateway(SimQueue& queue, Portfolio& portfolio, LatencyConfig latency) noexcept
        : queue_(queue), portfolio_(portfolio), latency_(latency) {}

    // Pre-trade risk first (Portfolio::reserve). Accepted: `id` gets the next
    // id and OrderArrive is queued at now + order_latency. Otherwise nothing
    // is reserved or queued and `id` is untouched.
    RiskCheck send(Side side, Price price, Quantity quantity, TimeInForce tif, OrderId& id) noexcept
    {
        const RiskCheck check = portfolio_.reserve(side, quantity);
        if (check != RiskCheck::Accepted) {
            return check;
        }
        id = next_id_++;
        queue_.push(SimEvent{.timestamp = now_ + latency_.order_latency, .type = SimEventType::OrderArrive,
                             .order = OrderRequest{.id = id, .side = side, .price = price, .quantity = quantity,
                                                   .time_in_force = tif}});
        return check;
    }

    // CancelArrive at now + order_latency, after any order sent before it.
    void cancel(OrderId id) noexcept
    {
        queue_.push(SimEvent{.timestamp = now_ + latency_.order_latency, .type = SimEventType::CancelArrive,
                             .order_id = id});
    }

    // Strategy::on_timer(timer_id) at `at`. False, and nothing queued, if `at` < now.
    bool set_timer(Timestamp at, std::uint64_t timer_id) noexcept
    {
        if (at < now_) {
            return false;
        }
        queue_.push(SimEvent{.timestamp = at, .type = SimEventType::Timer, .timer_id = timer_id});
        return true;
    }

    Timestamp now() const noexcept { return now_; }
    void set_now(Timestamp now) noexcept { now_ = now; }
};

} // namespace simulator
