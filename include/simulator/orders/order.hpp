#pragma once

#include <cstdint>

#include "simulator/types/enums.hpp"
#include "simulator/types/order_state.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/quantity.hpp"
#include "simulator/types/timestamp.hpp"

namespace simulator {

class PriceLevel;
using OrderId = std::uint64_t;

class Order
{
private:
    OrderId id_{0};
    Side side_{Side::Buy};
    Price price_{};
    Quantity quantity_{0};
    Quantity remaining_quantity_{0};
    OrderState state_{OrderState::New};
    Timestamp timestamp_{0};
    Order* prev_{nullptr};
    Order* next_{nullptr};
    PriceLevel* level_{nullptr};
    friend class PriceLevel;
public:
    constexpr Order(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity,
        Timestamp timestamp
    ) noexcept: id_(id),
          side_(side),
          price_(price),
          quantity_(quantity),
          remaining_quantity_(quantity),
          state_(OrderState::New),
          timestamp_(timestamp) {}

    bool activate() noexcept{
        if(state_ != OrderState::New){
            return false;
        }
        state_ = OrderState::Active;
        return true;
    }

    bool execute(Quantity executed) noexcept{

        if(executed.is_zero()) return false;

        if(state_ != OrderState::Active && state_ != OrderState::PartiallyFilled){
            return false;
        }

        if(executed > remaining_quantity_)return false;

        remaining_quantity_ = remaining_quantity_ - executed;

        if (remaining_quantity_.is_zero()) {
            state_ = OrderState::Filled;
        } else {
            state_ = OrderState::PartiallyFilled;
        }

        return true;
    }

    bool cancel() noexcept {
        if (state_ != OrderState::Active &&
            state_ != OrderState::PartiallyFilled) {
            return false;
        }

        state_ = OrderState::Cancelled;
        return true;
    }
     // Accessors

  // Accessors

    constexpr OrderId id() const noexcept {
        return id_;
    }

    constexpr Side side() const noexcept {
        return side_;
    }

    constexpr Price price() const noexcept {
        return price_;
    }

    constexpr Quantity quantity() const noexcept {
        return quantity_;
    }

    constexpr Quantity remaining_quantity() const noexcept {
        return remaining_quantity_;
    }

    constexpr OrderState state() const noexcept {
        return state_;
    }

    constexpr Timestamp timestamp() const noexcept {
        return timestamp_;
    }
};

} // namespace simulator