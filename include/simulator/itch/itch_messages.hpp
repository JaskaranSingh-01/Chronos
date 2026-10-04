#pragma once

#include <cstdint>

#include "simulator/types/enums.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/quantity.hpp"
#include "simulator/types/timestamp.hpp"
#include "simulator/types/order_id.hpp"

namespace simulator {

struct ItchAddOrder
{
    Timestamp timestamp{0};

    OrderId order_id{0};

    Side side{Side::Buy};

    Quantity quantity{0};

    Price price{0};
};

} // namespace simulator