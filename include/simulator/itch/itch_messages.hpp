#pragma once

#include <array>
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
    std::uint16_t stock_locate{0};
};

struct ItchOrderExecuted
{
    Timestamp timestamp{0};
    OrderId order_id{0};
    Quantity quantity{0};
    std::uint64_t match_number{0};
    std::uint16_t stock_locate{0};
};

struct ItchOrderExecutedWithPrice
{
    Timestamp timestamp{0};
    OrderId order_id{0};
    Quantity quantity{0};
    std::uint64_t match_number{0};
    bool printable{false};
    Price price{0};
    std::uint16_t stock_locate{0};
};

struct ItchOrderCancel
{
    Timestamp timestamp{0};
    OrderId order_id{0};
    Quantity quantity{0};
    std::uint16_t stock_locate{0};
};

struct ItchOrderDelete
{
    Timestamp timestamp{0};
    OrderId order_id{0};
    std::uint16_t stock_locate{0};
};

struct ItchOrderReplace
{
    Timestamp timestamp{0};
    OrderId original_order_id{0};
    OrderId replacement_order_id{0};
    Quantity quantity{0};
    Price price{0};
    std::uint16_t stock_locate{0};
};

/*
 * Maps a stock locate to its symbol. Not an order-book event, so it
 * is never normalised into a ReplayEvent.
 */
struct ItchStockDirectory
{
    Timestamp timestamp{0};
    std::uint16_t stock_locate{0};
    std::array<char, 8> stock{}; // raw, space-padded
};

} // namespace simulator