#pragma once

#include <cstdint>

#include "simulator/types/enums.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/quantity.hpp"
#include "simulator/types/timestamp.hpp"

namespace simulator {
using OrderId = std::uint64_t;

struct Event {
	Timestamp timestamp{0};
	EventType type{EventType::AddOrder};
	OrderId order_id{0};
	Side side{Side::Buy};
	Price price{};
	Quantity quantity{};
};
} // namespace simulator