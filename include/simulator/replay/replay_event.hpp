#pragma once

#include <cstdint>

#include "simulator/types/enums.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/quantity.hpp"
#include "simulator/types/timestamp.hpp"
#include "simulator/types/order_id.hpp"

namespace simulator {

enum class ReplayEventType : std::uint8_t {
    Add,
    Execute,
    Cancel,
    Delete,
    Replace
};

struct ReplayEvent
{
    Timestamp timestamp{0};

    ReplayEventType type{ReplayEventType::Add};

    // Existing order.
    OrderId order_id{0};

    // Used by Add / Replace.
    Side side{Side::Buy};

    // Used by Add / Replace.
    Price price{};

    // Add:
    //     new order quantity
    //
    // Execute:
    //     executed quantity
    //
    // Cancel:
    //     cancelled quantity
    //
    // Replace:
    //     new order quantity
    Quantity quantity{0};

    // Replace only.
    //
    // order_id              = old order
    // replacement_order_id = new order
    OrderId replacement_order_id{0};
};

} // namespace simulator