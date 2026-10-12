#pragma once

#include <cstddef>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

#include "simulator/sim/execution_report.hpp"

namespace simulator {

enum class SimEventType : std::uint8_t { OrderArrive, CancelArrive, ReportDeliver, Timer };

// Our side of the event stream; the market side is MarketEvent. A tagged
// struct dispatched with a switch: only the field `type` names is meaningful.
struct SimEvent
{
    Timestamp timestamp{0};
    std::uint64_t sequence{0};      // assigned by SimQueue::push
    SimEventType type{SimEventType::Timer};
    OrderRequest order{};           // OrderArrive
    OrderId order_id{0};            // CancelArrive
    ExecutionReport report{};       // ReportDeliver
    std::uint64_t timer_id{0};      // Timer
};

// Pending SimEvents, earliest first. Equal timestamps pop in push order, so
// two orders sent in one callback with equal latency arrive in call order.
class SimQueue
{
private:
    // std::priority_queue is a max-heap, so "less" means "later".
    struct Later
    {
        bool operator()(const SimEvent& a, const SimEvent& b) const noexcept
        {
            return a.timestamp != b.timestamp ? a.timestamp > b.timestamp : a.sequence > b.sequence;
        }
    };

    static std::vector<SimEvent> reserved(std::size_t capacity)
    {
        std::vector<SimEvent> events;
        events.reserve(capacity);
        return events;
    }

    std::priority_queue<SimEvent, std::vector<SimEvent>, Later> queue_;
    std::uint64_t next_sequence_{0};

public:
    // push allocates only once more than `capacity` events are pending.
    explicit SimQueue(std::size_t capacity)
        : queue_(Later{}, reserved(capacity)) {}

    // Overwrites event.sequence with the next push counter.
    void push(SimEvent event) noexcept
    {
        event.sequence = next_sequence_++;
        queue_.push(std::move(event));
    }

    bool empty() const noexcept { return queue_.empty(); }

    // Precondition: !empty().
    const SimEvent& top() const noexcept { return queue_.top(); }
    void pop() noexcept { queue_.pop(); }
};

} // namespace simulator
