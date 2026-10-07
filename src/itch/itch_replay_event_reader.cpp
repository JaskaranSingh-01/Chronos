#include "simulator/itch/itch_replay_event_reader.hpp"
#include "simulator/itch/itch_message_types.hpp"

namespace simulator {

ReplayReadResult ItchReplayReader::next(ReplayEvent& event) noexcept{
    while (true){
        ItchMessage message{};

        const ItchReadResult read_result = message_reader_.next(message);

        if (read_result == ItchReadResult::EndOfBuffer){
            return ReplayReadResult::EndOfFile;
        }

        if (read_result == ItchReadResult::Error){
            return ReplayReadResult::Error;
        }

        switch (message.type){

        case itch::ADD_ORDER:
        {
            ItchAddOrder decoded{};

            const ItchDecodeResult result = decoder_.decode_add_order(message,decoded);

            if (result != ItchDecodeResult::Success){
                return ReplayReadResult::Error;
            }

            event = normalizer_.normalize(decoded);

            return ReplayReadResult::Event;
        }

        case itch::ORDER_EXECUTED:
        {
            ItchOrderExecuted decoded{};

            const ItchDecodeResult result = decoder_.decode_order_executed(message,decoded);

            if (result != ItchDecodeResult::Success){
                return ReplayReadResult::Error;
            }

            event = normalizer_.normalize(decoded);

            return ReplayReadResult::Event;
        }

        case itch::ORDER_CANCEL:
        {
            ItchOrderCancel decoded{};

            const ItchDecodeResult result = decoder_.decode_order_cancel(message,decoded);

            if (result != ItchDecodeResult::Success){
                return ReplayReadResult::Error;
            }

            event = normalizer_.normalize(decoded);

            return ReplayReadResult::Event;
        }

        case itch::ORDER_DELETE:
        {
            ItchOrderDelete decoded{};

            const ItchDecodeResult result = decoder_.decode_order_delete(message,decoded);

            if (result != ItchDecodeResult::Success){
                return ReplayReadResult::Error;
            }

            event = normalizer_.normalize(decoded);

            return ReplayReadResult::Event;
        }

        default:
            /*
             * ITCH contains many message types that are not
             * required for order-book reconstruction.
             *
             * Ignore unsupported messages and continue reading.
             */
            continue;
        }
    }
}

} // namespace simulator