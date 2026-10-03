#include "simulator/orders/order.hpp"

namespace simulator {


    // Initialize / reuse an Order
    // --------------------------------------------------------

void Order::initialize(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity,
        Timestamp timestamp
    ) noexcept
    {
        id_ = id;
        side_ = side;
        price_ = price;

        quantity_ = quantity;
        remaining_quantity_ = quantity;

        state_ = OrderState::New;
        timestamp_ = timestamp;

        prev_ = nullptr;
        next_ = nullptr;
        level_ = nullptr;
    }

Order::Order(
    OrderId id,
    Side side,
    Price price,
    Quantity quantity,
    Timestamp timestamp
) noexcept
    : id_(id),
      side_(side),
      price_(price),
      quantity_(quantity),
      remaining_quantity_(quantity),
      state_(OrderState::New),
      timestamp_(timestamp) {}

bool Order::activate() noexcept {
    if (state_ != OrderState::New) {
        return false;
    }

    state_ = OrderState::Active;
    return true;
}

bool Order::execute(Quantity executed) noexcept {
    if (executed.is_zero()) {
        return false;
    }

    if (state_ != OrderState::Active &&
        state_ != OrderState::PartiallyFilled) {
        return false;
    }

    if (executed > remaining_quantity_) {
        return false;
    }

    remaining_quantity_ = remaining_quantity_ - executed;

    if (remaining_quantity_.is_zero()) {
        state_ = OrderState::Filled;
    } else {
        state_ = OrderState::PartiallyFilled;
    }

    return true;
}

bool Order::cancel() noexcept {
    if (state_ != OrderState::Active &&
        state_ != OrderState::PartiallyFilled) {
        return false;
    }

    state_ = OrderState::Cancelled;
    return true;
}

// Accessors

OrderId Order::id() const noexcept {
    return id_;
}

Side Order::side() const noexcept {
    return side_;
}

Price Order::price() const noexcept {
    return price_;
}

Quantity Order::quantity() const noexcept {
    return quantity_;
}

Quantity Order::remaining_quantity() const noexcept {
    return remaining_quantity_;
}

OrderState Order::state() const noexcept {
    return state_;
}

Timestamp Order::timestamp() const noexcept {
    return timestamp_;
}

} // namespace simulator