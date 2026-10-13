#include "simulator/orders/order_book.hpp"

namespace simulator {

// Detaches the order from its price level and drops the level once it
// is empty. Leaves orders_ and the order's state to the caller.
// level_ already points at the order's level, so the map is searched only
// to erase a level that emptied (M2.12).
bool OrderBook::unlink(Order& order) noexcept
{
    PriceLevel* level = order.level_;

    if (level == nullptr || !level->remove(order)) {
        return false;
    }

    if (level->empty()) {
        if (order.side() == Side::Buy) {
            bids_.erase(level->price());
        } else {
            asks_.erase(level->price());
        }
    }

    return true;
}

bool OrderBook::add(Order& order) noexcept
{
    if(order.remaining_quantity().is_zero()){
        return false;
    }

    if (order.state() != OrderState::Active &&
        order.state() != OrderState::PartiallyFilled) {
        return false;
    }

    // Order ID must be unique.
    if (orders_.find(order.id()) != nullptr) {
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
    orders_.insert(order.id(), &order);

    return true;
}

bool OrderBook::remove(OrderId id) noexcept{
    Order* order = orders_.find(id);

    if(order == nullptr){
        return false;
    }
    
    if (order->state() != OrderState::Filled){
        return false;
    }
    
    if (!unlink(*order)) {
        return false;
    }

    orders_.erase(id);

    return true;

}

bool OrderBook::cancel(OrderId id) noexcept
{
    Order* order = orders_.find(id);

    if (order == nullptr) {
        return false;
    }

    if (order->state() != OrderState::Active &&
        order->state() != OrderState::PartiallyFilled) {
        return false;
    }

    if (!unlink(*order)) {
        return false;
    }

    // Now that the order is no longer in the book,
    // transition its lifecycle state.
    if (!order->cancel()) {
        return false;
    }

    orders_.erase(id);

    return true;
}

Order* OrderBook::find(OrderId id) noexcept
{
    return orders_.find(id);
}

const Order* OrderBook::find(OrderId id) const noexcept
{
    return orders_.find(id);
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
    return orders_.size() == 0;
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

bool OrderBook::execute(
    OrderId id,
    Quantity quantity
) noexcept
{
    Order* order = orders_.find(id);

    if (order == nullptr) {
        return false;
    }

    if (!order->execute(quantity)) {
        return false;
    }

    order->level_->reduce(quantity);

    if (order->state() != OrderState::Filled) {
        return true;
    }

    if (!unlink(*order)) {
        return false;
    }

    orders_.erase(id);

    return true;
}

bool OrderBook::cancel(
    OrderId id,
    Quantity quantity
) noexcept
{
    Order* order = orders_.find(id);

    if (order == nullptr) {
        return false;
    }

    if (!order->cancel(quantity)) {
        return false;
    }

    order->level_->reduce(quantity);

    if (order->state() != OrderState::Cancelled) {
        return true;
    }

    if (!unlink(*order)) {
        return false;
    }

    orders_.erase(id);

    return true;
}

bool OrderBook::erase(OrderId id) noexcept
{
    Order* order = orders_.find(id);

    if (order == nullptr) {
        return false;
    }

    if (!unlink(*order)) {
        return false;
    }

    orders_.erase(id);

    return true;
}

} // namespace simulator