#include "simulator/itch/itch_replay_reader.hpp"

namespace simulator {

bool ItchReplayReader::next(
    ReplayEvent& event
) noexcept
{
    ItchMessage message;

    if (!message_reader_.next(message))
    {
        return false;
    }

    switch (message.type)
    {
    case ItchDecoder::ADD_ORDER:
    {
        ItchAddOrder decoded{};

        const ItchDecoder::Result result =
            decoder_.decode_add_order(
                message,
                decoded
            );

        if (result != ItchDecoder::Result::Success)
        {
            return false;
        }

        event = normalizer_.normalize(decoded);

        return true;
    }

    case ItchDecoder::ORDER_EXECUTED:
    {
        ItchOrderExecuted decoded{};

        const ItchDecoder::Result result =
            decoder_.decode_order_executed(
                message,
                decoded
            );

        if (result != ItchDecoder::Result::Success)
        {
            return false;
        }

        event = normalizer_.normalize(decoded);

        return true;
    }

    case ItchDecoder::ORDER_CANCEL:
    {
        ItchOrderCancel decoded{};

        const ItchDecoder::Result result =
            decoder_.decode_order_cancel(
                message,
                decoded
            );

        if (result != ItchDecoder::Result::Success)
        {
            return false;
        }

        event = normalizer_.normalize(decoded);

        return true;
    }

    case ItchDecoder::ORDER_DELETE:
    {
        ItchOrderDelete decoded{};

        const ItchDecoder::Result result =
            decoder_.decode_order_delete(
                message,
                decoded
            );

        if (result != ItchDecoder::Result::Success)
        {
            return false;
        }

        event = normalizer_.normalize(decoded);

        return true;
    }

    default:
        return false;
    }
}

} // namespace simulator