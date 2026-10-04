#include "simulator/replay/replay_engine.hpp"

namespace simulator {

std::size_t ReplayEngine::replay(
    const std::vector<ReplayEvent>& events
)
{
    std::size_t processed = 0;

    for (const ReplayEvent& event : events)
    {
        simulator_.advance_to(event.timestamp);

        switch (event.type)
        {
        case ReplayEventType::Add:
        {
            simulator_.submit_order(
                event.order_id,
                event.side,
                event.price,
                event.quantity,
                event.timestamp
            );

            break;
        }

        case ReplayEventType::Cancel:
        {
            // Cancellation API will be added next.
            break;
        }
        }

        ++processed;
    }

    return processed;
}

} // namespace simulator