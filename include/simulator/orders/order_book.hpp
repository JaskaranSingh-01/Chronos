#pragma once

#include <cstddef>
#include <functional>
#include <map>

#include "simulator/orders/order.hpp"
#include "simulator/orders/order_index.hpp"
#include "simulator/orders/price_level.hpp"
#include "simulator/types/order_id.hpp"
namespace simulator
{
    class OrderBook
    {
    public:
        // Best price first on both sides.
        using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
        using AskLevels = std::map<Price, PriceLevel>;

    private:
        BidLevels bids_;
        AskLevels asks_;
        OrderIndex orders_;

        bool unlink(Order &order) noexcept;

    public:
        // Room in the order index for `capacity` orders before it grows: a
        // book that owns its OrderPool passes the pool's capacity.
        explicit OrderBook(std::size_t capacity = 0) : orders_(capacity) {}

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
        std::size_t size() const noexcept { return orders_.size(); }
        const BidLevels& bids() const noexcept { return bids_; }
        const AskLevels& asks() const noexcept { return asks_; }
        // void clear() noexcept;

        bool execute(OrderId id, Quantity quantity) noexcept;
        bool cancel(OrderId id, Quantity quantity) noexcept;
        bool erase(OrderId id) noexcept;
    };

} // namespace simulator