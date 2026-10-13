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
    // Price levels and an id index over Orders it links but does not own.
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
        // Sizes the order index for `capacity` orders before it grows.
        explicit OrderBook(std::size_t capacity = 0) : orders_(capacity) {}

        bool add(Order &order) noexcept;        
        bool cancel(OrderId id) noexcept;
        Order *find(OrderId id) noexcept;
        const Order *find(OrderId id) const noexcept;
        const Order *best_bid() const noexcept;
        const Order *best_ask() const noexcept;
        bool empty() const noexcept;
        std::size_t size() const noexcept { return orders_.size(); }
        const BidLevels& bids() const noexcept { return bids_; }
        const AskLevels& asks() const noexcept { return asks_; }

        // False if `id` is unknown or the order rejects the change.
        // An order reduced to zero leaves the book.
        bool execute(OrderId id, Quantity quantity) noexcept;
        bool cancel(OrderId id, Quantity quantity) noexcept;
        bool erase(OrderId id) noexcept;
    };

} // namespace simulator