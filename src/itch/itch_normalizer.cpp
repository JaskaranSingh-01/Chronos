#include "simulator/itch/itch_normalizer.hpp"

namespace simulator {

ReplayEvent ItchNormalizer::normalize(
    const ItchAddOrder& order
) const noexcept
{
    ReplayEvent event;

    event.timestamp =
        order.timestamp;

    event.type =
        ReplayEventType::Add;

    event.order_id =
        order.order_id;

    event.side =
        order.side;

    event.price =
        order.price;

    event.quantity =
        order.quantity;

    return event;
}

} // namespace simulator