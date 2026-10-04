#include "simulator/replay/replay_engine.hpp"

namespace simulator {

std::size_t ReplayEngine::process_event(
    const ReplayEvent& event
)
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

        simulator_.release_filled_orders();
        break;
    }

    case ReplayEventType::Cancel:
    {
        simulator_.cancel_order(
            event.order_id
        );

        break;
    }
    }

    return 1;
}

std::size_t ReplayEngine::replay(
    const std::vector<ReplayEvent>& events
)
{
    std::size_t processed = 0;

    for (const ReplayEvent& event : events) {
        processed += process_event(event);
    }

    return processed;
}

std::size_t ReplayEngine::replay(
    ReplayEventReader& reader
)
{
    std::size_t processed = 0;

    ReplayEvent event;

    while (true) {
        const ReplayReadResult result =
            reader.next(event);

        if (result == ReplayReadResult::EndOfFile) {
            break;
        }

        if (result == ReplayReadResult::Error) {
            break;
        }

        processed += process_event(event);
    }

    return processed;
}

} // namespace simulator
