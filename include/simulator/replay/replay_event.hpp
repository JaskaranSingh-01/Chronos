#pragma once

#include <cstdint>

#include "simulator/types/enums.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/quantity.hpp"
#include "simulator/types/timestamp.hpp"

namespace simulator {

enum class ReplayEventType : std::uint8_t {
    Add,
    Cancel
};

struct ReplayEvent
{
    Timestamp timestamp{0};

    ReplayEventType type{ReplayEventType::Add};

    OrderId order_id{0};

    Side side{Side::Buy};

    Price price{};

    Quantity quantity{};
};

} // namespace simulator