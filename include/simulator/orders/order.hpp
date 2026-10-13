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
    Price price_{};
    Quantity quantity_{0};
    Quantity remaining_quantity_{0};
    Timestamp timestamp_{0};
    std::uint64_t sequence_{0};  // arrival order, assigned by MarketBook

    Order* prev_{nullptr};
    Order* next_{nullptr};
    PriceLevel* level_{nullptr};

    // 1-byte fields last so an Order packs into 80 bytes.
    Side side_{Side::Buy};
    OrderState state_{OrderState::New};

    friend class PriceLevel;
    friend class OrderPool;
    friend class OrderBook;  // reduces level_->total_quantity on partial changes


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

    // State transitions: false, changing nothing, if the state or quantity forbids it.
    bool activate() noexcept;

    bool execute(Quantity executed) noexcept;

    bool cancel() noexcept;
    bool cancel(Quantity cancelled) noexcept;
    bool delete_order() noexcept;

    OrderId id() const noexcept;
    Side side() const noexcept;
    Price price() const noexcept;
    Quantity quantity() const noexcept;
    Quantity remaining_quantity() const noexcept;
    OrderState state() const noexcept;
    Timestamp timestamp() const noexcept;

    std::uint64_t sequence() const noexcept { return sequence_; }
    void set_sequence(std::uint64_t sequence) noexcept { sequence_ = sequence; }

    // Read-only view of the order's place in the book.
    const Order* next() const noexcept { return next_; }
    const PriceLevel* level() const noexcept { return level_; }
};

} // namespace simulator