#include "simulator/replay/replay_engine.hpp"

namespace simulator {

ReplayEngine::ProcessResult ReplayEngine::process_event(
    const ReplayEvent& event,
    ReplayStatistics& statistics
)
{
    switch (event.type)
    {
    case ReplayEventType::Add:
    {
        const std::size_t trades_before =
            simulator_.trades().size();

        Order* order = simulator_.submit_order(
            event.order_id,
            event.side,
            event.price,
            event.quantity,
            event.timestamp
        );

        if (order == nullptr) {
            return ProcessResult::Rejected;
        }

        simulator_.release_filled_orders();

        ++statistics.add_events;
        statistics.trades +=
            simulator_.trades().size() - trades_before;

        return ProcessResult::Processed;
    }

    case ReplayEventType::Execute:
    {
        if (!simulator_.replay_execute_order(event.order_id,event.quantity)){
            return ProcessResult::Rejected;
        }

        ++statistics.execute_events;
        return ProcessResult::Processed;
    }

    case ReplayEventType::Cancel:
    {
        if (!simulator_.cancel_order(event.order_id)) {
            return ProcessResult::Rejected;
        }

        ++statistics.cancel_events;
        return ProcessResult::Processed;
    }

    case ReplayEventType::Delete:
    {
        if (!simulator_.replay_delete_order(event.order_id)) {
            return ProcessResult::Rejected;
        }
        ++statistics.delete_events;
        return ProcessResult::Processed;
    }

    case ReplayEventType::Replace:
    {
        // Not implemented yet.
        //
        // Replace semantics will be added after the normalized
        // event model and ITCH message definitions are established.

        return ProcessResult::Rejected;
    }
    }

    return ProcessResult::Rejected;
}

ReplayResult ReplayEngine::replay(
    const std::vector<ReplayEvent>& events
)
{
    ReplayResult result;

    bool has_previous_timestamp = false;
    Timestamp previous_timestamp{0};

    for (std::size_t index = 0; index < events.size(); ++index)
    {
        const ReplayEvent& event = events[index];

        ++result.statistics.received_events;

        if (has_previous_timestamp &&
            event.timestamp < previous_timestamp)
        {
            result.failure =
                ReplayFailure::OutOfOrderTimestamp;

            result.failed_event_index = index + 1;
            return result;
        }

        has_previous_timestamp = true;
        previous_timestamp = event.timestamp;

        if (process_event(event, result.statistics) ==
            ProcessResult::Rejected)
        {
            ++result.statistics.rejected_events;
            continue;
        }

        simulator_.advance_to(event.timestamp);
        ++result.statistics.processed_events;
    }

    return result;
}

ReplayResult ReplayEngine::replay(
    ReplayEventReader& reader
)
{
    ReplayResult result;

    bool has_previous_timestamp = false;
    Timestamp previous_timestamp{0};

    ReplayEvent event;

    while (true)
    {
        const ReplayReadResult read_result =
            reader.next(event);

        if (read_result == ReplayReadResult::EndOfFile) {
            return result;
        }

        if (read_result == ReplayReadResult::Error)
        {
            result.failure = ReplayFailure::ReaderError;
            result.failed_event_index =
                result.statistics.received_events + 1;
            result.failed_source_line = reader.line_number();

            return result;
        }

        ++result.statistics.received_events;

        if (has_previous_timestamp &&
            event.timestamp < previous_timestamp)
        {
            result.failure =
                ReplayFailure::OutOfOrderTimestamp;

            result.failed_event_index =
                result.statistics.received_events;

            result.failed_source_line =
                reader.line_number();

            return result;
        }

        has_previous_timestamp = true;
        previous_timestamp = event.timestamp;

        if (process_event(event, result.statistics) ==
            ProcessResult::Rejected)
        {
            ++result.statistics.rejected_events;
            continue;
        }

        simulator_.advance_to(event.timestamp);
        ++result.statistics.processed_events;
    }
}

} // namespace simulator