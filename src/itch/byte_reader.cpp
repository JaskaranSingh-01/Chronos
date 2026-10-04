#include "simulator/itch/byte_reader.hpp"

namespace simulator {

bool ByteReader::read_u8(std::uint8_t& value) noexcept
{
    if (remaining() < 1) {
        return false;
    }

    value = std::to_integer<std::uint8_t>(data_[position_]);

    ++position_;

    return true;
}

bool ByteReader::read_u16_be(std::uint16_t& value) noexcept
{
    if (remaining() < 2) {
        return false;
    }

    const std::uint16_t byte_0 = std::to_integer<std::uint8_t>(data_[position_]);

    const std::uint16_t byte_1 = std::to_integer<std::uint8_t>(data_[position_ + 1]);

    value =
        static_cast<std::uint16_t>(
            (byte_0 << 8) |
            byte_1
        );

    position_ += 2;

    return true;
}

bool ByteReader::read_u32_be(std::uint32_t& value) noexcept
{
    if (remaining() < 4) {
        return false;
    }

    const std::uint32_t byte_0 = std::to_integer<std::uint8_t>(data_[position_]);

    const std::uint32_t byte_1 = std::to_integer<std::uint8_t>(data_[position_ + 1]);

    const std::uint32_t byte_2 = std::to_integer<std::uint8_t>(data_[position_ + 2]);

    const std::uint32_t byte_3 = std::to_integer<std::uint8_t>(data_[position_ + 3]);

    value =
        (byte_0 << 24) |
        (byte_1 << 16) |
        (byte_2 << 8) |
        byte_3;

    position_ += 4;

    return true;
}

bool ByteReader::read_u48_be(std::uint64_t& value) noexcept
{
    if (remaining() < 6) {
        return false;
    }

    const std::uint64_t byte_0 = std::to_integer<std::uint8_t>(data_[position_]);

    const std::uint64_t byte_1 = std::to_integer<std::uint8_t>(data_[position_ + 1]);

    const std::uint64_t byte_2 = std::to_integer<std::uint8_t>(data_[position_ + 2]);

    const std::uint64_t byte_3 = std::to_integer<std::uint8_t>(data_[position_ + 3]);

    const std::uint64_t byte_4 = std::to_integer<std::uint8_t>(data_[position_ + 4]);

    const std::uint64_t byte_5 = std::to_integer<std::uint8_t>(data_[position_ + 5]);

    value =
        (byte_0 << 40) |
        (byte_1 << 32) |
        (byte_2 << 24) |
        (byte_3 << 16) |
        (byte_4 << 8) |
        byte_5;

    position_ += 6;

    return true;
}

bool ByteReader::read_u64_be(std::uint64_t& value) noexcept
{
    if (remaining() < 8) {
        return false;
    }

    const std::uint64_t byte_0 = std::to_integer<std::uint8_t>(data_[position_]);

    const std::uint64_t byte_1 = std::to_integer<std::uint8_t>(data_[position_ + 1]);

    const std::uint64_t byte_2 = std::to_integer<std::uint8_t>(data_[position_ + 2]);

    const std::uint64_t byte_3 = std::to_integer<std::uint8_t>(data_[position_ + 3]);

    const std::uint64_t byte_4 = std::to_integer<std::uint8_t>(data_[position_ + 4]);

    const std::uint64_t byte_5 = std::to_integer<std::uint8_t>(data_[position_ + 5]);

    const std::uint64_t byte_6 = std::to_integer<std::uint8_t>(data_[position_ + 6]);

    const std::uint64_t byte_7 = std::to_integer<std::uint8_t>(data_[position_ + 7]);

    value =
        (byte_0 << 56) |
        (byte_1 << 48) |
        (byte_2 << 40) |
        (byte_3 << 32) |
        (byte_4 << 24) |
        (byte_5 << 16) |
        (byte_6 << 8) |
        byte_7;

    position_ += 8;

    return true;
}

bool ByteReader::read_bytes(std::size_t count, std::span<const std::byte>& bytes) noexcept
{
    if (count > remaining()) {
        return false;
    }

    bytes = data_.subspan(position_,count);

    position_ += count;

    return true;
}

} // namespace simulator