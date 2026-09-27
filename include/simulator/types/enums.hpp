#pragma once

#include <cstdint> 

namespace simulator
{
    enum class Side : std::uint8_t{
        Buy,
        Sell
    };

    enum class OrderType : std::uint8_t{
        Market,
        Limit
    };
    
    enum class EventType : std::uint8_t{
        AddOrder,
        CancelOrder,
        ExecuteOrder,
        Trade
    };
} // namespace simulator
