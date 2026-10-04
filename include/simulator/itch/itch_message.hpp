#pragma once

#include <cstdint>
#include <span>

namespace simulator {

struct ItchMessage
{
    std::uint8_t type{0};

    std::span<const std::byte> payload{};
};

} // namespace simulator