#pragma once

#include <functional>
#include <map>
#include <unordered_map>

#include "simulator/orders/order.hpp"
#include "simulator/orders/price_level.hpp"
#include "simulator/types/order_id.hpp"
namespace simulator
{
    class OrderBook
    {
    private:
        std::map<Price, PriceLevel, std::greater<Price>> bids_;
        std::map<Price, PriceLevel> asks_;
        std::unordered_map<OrderId, Order *> orders_;

    public:
        bool add(Order &order) noexcept;
        
        bool remove(OrderId id) noexcept;

        bool cancel(OrderId id) noexcept;

        Order *find(OrderId id) noexcept;

        const Order *find(OrderId id) const noexcept;

        Order *best_bid() noexcept;
        Order *best_ask() noexcept;

        const Order *best_bid() const noexcept;
        const Order *best_ask() const noexcept;

        bool empty() const noexcept;

        // void clear() noexcept;
    };

} // namespace simulator