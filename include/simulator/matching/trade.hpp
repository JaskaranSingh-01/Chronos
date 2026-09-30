#pragma once

#include "simulator/orders/order.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/quantity.hpp"
#include "simulator/types/timestamp.hpp"

namespace simulator {

struct Trade
{
    OrderId buy_order_id;
    OrderId sell_order_id;

    Price price;
    Quantity quantity;

    Timestamp timestamp;
};

} // namespace simulator