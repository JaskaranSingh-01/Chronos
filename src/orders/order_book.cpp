#include "simulator/orders/order_book.hpp"

namespace simulator {

bool OrderBook::add(Order& order) noexcept
{
    // Only active orders can enter the book.
    if (order.state() != OrderState::Active) {
        return false;
    }

    // Order ID must be unique.
    if (orders_.find(order.id()) != orders_.end()) {
        return false;
    }

    if (order.side() == Side::Buy) {

        auto [it, inserted] =
            bids_.try_emplace(
                order.price(),
                order.price()
            );

        PriceLevel& level = it->second;

        if (!level.add(order)) {
            // If we created a new empty level but failed
            // to add the order, remove that level.
            if (inserted && level.empty()) {
                bids_.erase(it);
            }

            return false;
        }

    } else {

        auto [it, inserted] =
            asks_.try_emplace(
                order.price(),
                order.price()
            );

        PriceLevel& level = it->second;

        if (!level.add(order)) {
            if (inserted && level.empty()) {
                asks_.erase(it);
            }

            return false;
        }
    }

    // Order successfully entered the book.
    orders_.emplace(order.id(), &order);

    return true;
}

bool OrderBook::remove(OrderId id) noexcept{
    auto it = orders_.find(id);

    if(it == orders_.end()){
        return false;
    }

    Order* order = it->second;
    if(order == nullptr){
        return false;
    }
    
    if (order->state() != OrderState::Filled){
        return false;
    }
    
    const Price price = order->price();

    if (order->side() == Side::Buy) {

        auto level_it = bids_.find(price);

        if (level_it == bids_.end()) {
            return false;
        }

        PriceLevel& level = level_it->second;

        if (!level.remove(*order)) {
            return false;
        }

        if (level.empty()) {
            bids_.erase(level_it);
        }

    } else {

        auto level_it = asks_.find(price);

        if (level_it == asks_.end()) {
            return false;
        }

        PriceLevel& level = level_it->second;

        if (!level.remove(*order)) {
            return false;
        }

        if (level.empty()) {
            asks_.erase(level_it);
        }
    }

    orders_.erase(it);

    return true;

}

bool OrderBook::cancel(OrderId id) noexcept
{
    auto it = orders_.find(id);

    if (it == orders_.end()) {
        return false;
    }

    Order* order = it->second;

    if (order == nullptr) {
        return false;
    }

    if (order->state() != OrderState::Active &&
        order->state() != OrderState::PartiallyFilled) {
        return false;
    }

    const Price price = order->price();

    if (order->side() == Side::Buy) {

        auto level_it = bids_.find(price);

        if (level_it == bids_.end()) {
            return false;
        }

        PriceLevel& level = level_it->second;

        if (!level.remove(*order)) {
            return false;
        }

        if (level.empty()) {
            bids_.erase(level_it);
        }

    } else {

        auto level_it = asks_.find(price);

        if (level_it == asks_.end()) {
            return false;
        }

        PriceLevel& level = level_it->second;

        if (!level.remove(*order)) {
            return false;
        }

        if (level.empty()) {
            asks_.erase(level_it);
        }
    }

    // Now that the order is no longer in the book,
    // transition its lifecycle state.
    if (!order->cancel()) {
        return false;
    }

    orders_.erase(it);

    return true;
}

Order* OrderBook::find(OrderId id) noexcept
{
    auto it = orders_.find(id);

    if (it == orders_.end()) {
        return nullptr;
    }

    return it->second;
}

const Order* OrderBook::find(OrderId id) const noexcept
{
    auto it = orders_.find(id);

    if (it == orders_.end()) {
        return nullptr;
    }

    return it->second;
}

Order* OrderBook::best_bid() noexcept
{
    if (bids_.empty()) {
        return nullptr;
    }

    return bids_.begin()->second.front();
}

Order* OrderBook::best_ask() noexcept
{
    if (asks_.empty()) {
        return nullptr;
    }

    return asks_.begin()->second.front();
}

const Order* OrderBook::best_bid() const noexcept
{
    if (bids_.empty()) {
        return nullptr;
    }

    return bids_.begin()->second.front();
}

const Order* OrderBook::best_ask() const noexcept
{
    if (asks_.empty()) {
        return nullptr;
    }

    return asks_.begin()->second.front();
}

bool OrderBook::empty() const noexcept
{
    return orders_.empty();
}

// void OrderBook::clear() noexcept
// {
//     bids_.clear();
//     asks_.clear();
//     orders_.clear();
// }
// this creates dangling pointers

// void OrderBook::clear() noexcept
// {
//     while (!orders_.empty()) {
//         auto it = orders_.begin();

//         Order* order = it->second;

//         if (order == nullptr) {
//             orders_.erase(it);
//             continue;
//         }

//         if (order->side() == Side::Buy) {
//             auto level_it = bids_.find(order->price());

//             if (level_it != bids_.end()) {
//                 level_it->second.remove(*order);

//                 if (level_it->second.empty()) {
//                     bids_.erase(level_it);
//                 }
//             }
//         } else {
//             auto level_it = asks_.find(order->price());

//             if (level_it != asks_.end()) {
//                 level_it->second.remove(*order);

//                 if (level_it->second.empty()) {
//                     asks_.erase(level_it);
//                 }
//             }
//         }

//         orders_.erase(it);
//     }
// }

} // namespace simulator