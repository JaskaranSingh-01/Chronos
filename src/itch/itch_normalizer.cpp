#include "simulator/itch/itch_normalizer.hpp"

namespace simulator {

ReplayEvent ItchNormalizer::normalize(const ItchAddOrder& order) const noexcept{
    ReplayEvent event;

    event.timestamp = order.timestamp;
    event.type = ReplayEventType::Add;
    event.order_id = order.order_id;
    event.side = order.side;
    event.price = order.price;
    event.quantity = order.quantity;

    return event;
}


ReplayEvent ItchNormalizer::normalize(const ItchOrderExecuted& order) const noexcept{
    ReplayEvent event;

    event.timestamp = order.timestamp;
    event.type = ReplayEventType::Execute;
    event.order_id = order.order_id;
    event.quantity = order.quantity;

    return event;
}


ReplayEvent ItchNormalizer::normalize(const ItchOrderCancel& order) const noexcept{
    ReplayEvent event;

    event.timestamp = order.timestamp;
    event.type = ReplayEventType::Cancel;
    event.order_id = order.order_id;
    event.quantity = order.quantity;

    return event;
}


ReplayEvent ItchNormalizer::normalize(const ItchOrderDelete& order) const noexcept{
    ReplayEvent event;

    event.timestamp = order.timestamp;
    event.type = ReplayEventType::Delete;
    event.order_id = order.order_id;

    return event;
}
ReplayEvent ItchNormalizer::normalize(const ItchOrderReplace& order) const noexcept{
    ReplayEvent event;

    event.timestamp = order.timestamp;
    event.type = ReplayEventType::Replace;
    event.order_id = order.original_order_id;
    event.replacement_order_id = order.replacement_order_id;
    event.quantity = order.quantity;
    event.price = order.price;

    return event;
}

} // namespace simulator