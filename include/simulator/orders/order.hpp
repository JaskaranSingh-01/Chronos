#pragma once

#include <cstdint>

#include "simulator/types/enums.hpp"
#include "simulator/types/order_state.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/quantity.hpp"
#include "simulator/types/timestamp.hpp"
#include "simulator/types/order_id.hpp"

namespace simulator {

class PriceLevel;
class OrderPool;

class Order {
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
    friend class OrderPool;


public:
    Order() noexcept = default;

    Order(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity,
        Timestamp timestamp
    ) noexcept;

    void initialize(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity,
        Timestamp timestamp
    ) noexcept;

    bool activate() noexcept;

    bool execute(Quantity executed) noexcept;

    bool cancel() noexcept;
    bool cancel(Quantity cancelled) noexcept;
    bool delete_order() noexcept;

    // Accessors
    OrderId id() const noexcept;
    Side side() const noexcept;
    Price price() const noexcept;
    Quantity quantity() const noexcept;
    Quantity remaining_quantity() const noexcept;
    OrderState state() const noexcept;
    Timestamp timestamp() const noexcept;
};

} // namespace simulator