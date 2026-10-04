#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include "simulator/types/order_id.hpp"


namespace simulator {

class ByteReader
{
private:
    std::span<const std::byte> data_;
    std::size_t position_{0};

public:
    explicit ByteReader(
        std::span<const std::byte> data
    ) noexcept
        : data_(data)
    {}

    bool read_u8(
        std::uint8_t& value
    ) noexcept;

    bool read_u16_be(
        std::uint16_t& value
    ) noexcept;

    bool read_u32_be(
        std::uint32_t& value
    ) noexcept;

    bool read_u48_be(
        std::uint64_t& value
    ) noexcept;

    bool read_u64_be(
        std::uint64_t& value
    ) noexcept;
    

    bool read_bytes(
        std::size_t count,
        std::span<const std::byte>& bytes
    ) noexcept;

    constexpr std::size_t position() const noexcept
    {
        return position_;
    }

    constexpr std::size_t remaining() const noexcept
    {
        return data_.size() - position_;
    }

    constexpr bool empty() const noexcept
    {
        return position_ == data_.size();
    }
};

} // namespace simulator