#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace simulator::itch_test {

// Builds wire-exact NASDAQ TotalView-ITCH 5.0 messages, each framed as
// [u16 big-endian length][message]. Every message starts with
// type, stock_locate u16, tracking_number u16, timestamp u48 (ns since midnight).
class ItchBuilder
{
public:
    // Tracking number for subsequent messages (default 0).
    ItchBuilder& tracking(std::uint16_t value)
    {
        tracking_ = value;
        return *this;
    }

    // 'A' Add Order (36 bytes).
    ItchBuilder& add_order(std::uint16_t locate, std::uint64_t ts, std::uint64_t order_id, char side,
                           std::uint32_t shares, std::string_view stock, std::uint32_t price)
    {
        const auto start = begin('A', locate, ts);
        put(order_id, 8);
        put_char(side);
        put(shares, 4);
        put_alpha(stock, 8);
        put(price, 4);
        return end(start);
    }

    // 'F' Add Order with MPID attribution (40 bytes).
    ItchBuilder& add_order_mpid(std::uint16_t locate, std::uint64_t ts, std::uint64_t order_id, char side,
                                std::uint32_t shares, std::string_view stock, std::uint32_t price,
                                std::string_view mpid)
    {
        const auto start = begin('F', locate, ts);
        put(order_id, 8);
        put_char(side);
        put(shares, 4);
        put_alpha(stock, 8);
        put(price, 4);
        put_alpha(mpid, 4);
        return end(start);
    }

    // 'E' Order Executed (31 bytes).
    ItchBuilder& executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t order_id, std::uint32_t shares,
                          std::uint64_t match)
    {
        const auto start = begin('E', locate, ts);
        put(order_id, 8);
        put(shares, 4);
        put(match, 8);
        return end(start);
    }

    // 'C' Order Executed With Price (36 bytes).
    ItchBuilder& executed_with_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t order_id,
                                     std::uint32_t shares, std::uint64_t match, bool printable, std::uint32_t price)
    {
        const auto start = begin('C', locate, ts);
        put(order_id, 8);
        put(shares, 4);
        put(match, 8);
        put_char(printable ? 'Y' : 'N');
        put(price, 4);
        return end(start);
    }

    // 'X' Order Cancel (23 bytes).
    ItchBuilder& cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t order_id, std::uint32_t shares)
    {
        const auto start = begin('X', locate, ts);
        put(order_id, 8);
        put(shares, 4);
        return end(start);
    }

    // 'D' Order Delete (19 bytes).
    ItchBuilder& erase(std::uint16_t locate, std::uint64_t ts, std::uint64_t order_id)
    {
        const auto start = begin('D', locate, ts);
        put(order_id, 8);
        return end(start);
    }

    // 'U' Order Replace (35 bytes).
    ItchBuilder& replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_id, std::uint64_t new_id,
                         std::uint32_t shares, std::uint32_t price)
    {
        const auto start = begin('U', locate, ts);
        put(old_id, 8);
        put(new_id, 8);
        put(shares, 4);
        put(price, 4);
        return end(start);
    }

    // 'P' Trade, non-cross (44 bytes).
    ItchBuilder& trade(std::uint16_t locate, std::uint64_t ts, std::uint64_t order_id, char side, std::uint32_t shares,
                       std::string_view stock, std::uint32_t price, std::uint64_t match)
    {
        const auto start = begin('P', locate, ts);
        put(order_id, 8);
        put_char(side);
        put(shares, 4);
        put_alpha(stock, 8);
        put(price, 4);
        put(match, 8);
        return end(start);
    }

    // 'H' Stock Trading Action (25 bytes). state: 'H' halted, 'P' paused,
    // 'Q' quotation only, 'T' trading. The reserved byte is ' ', as in real files.
    ItchBuilder& trading_action(std::uint16_t locate, std::uint64_t ts, std::string_view stock, char state,
                                std::string_view reason = "")
    {
        const auto start = begin('H', locate, ts);
        put_alpha(stock, 8);
        put_char(state);
        put_char(' ');  // reserved
        put_alpha(reason, 4);
        return end(start);
    }

    // 'S' System Event (12 bytes). Chronos doesn't decode it; tests use it as
    // a real message ItchSource must skip.
    ItchBuilder& system_event(std::uint16_t locate, std::uint64_t ts, char event_code)
    {
        const auto start = begin('S', locate, ts);
        put_char(event_code);
        return end(start);
    }

    // 'R' Stock Directory (39 bytes). Only locate and stock matter to Chronos;
    // the remaining fields get fixed plausible values.
    ItchBuilder& stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view stock)
    {
        const auto start = begin('R', locate, ts);
        put_alpha(stock, 8);
        put_char(' '); // market category
        put_char(' '); // financial status indicator
        put(100, 4);   // round lot size
        put_char('N'); // round lots only
        put_char(' '); // issue classification
        put_alpha("", 2); // issue sub-type
        put_char('P'); // authenticity: production
        put_char(' '); // short sale threshold indicator
        put_char(' '); // IPO flag
        put_char(' '); // LULD reference price tier
        put_char(' '); // ETP flag
        put(0, 4);     // ETP leverage factor
        put_char('N'); // inverse indicator
        return end(start);
    }

    std::span<const std::byte> bytes() const noexcept
    {
        return bytes_;
    }

private:
    // Reserves the length prefix and writes the common header; returns the
    // frame start so end() can back-fill the length.
    std::size_t begin(char type, std::uint16_t locate, std::uint64_t ts)
    {
        const auto start = bytes_.size();
        put(0, 2);
        put_char(type);
        put(locate, 2);
        put(tracking_, 2);
        put(ts, 6);
        return start;
    }

    ItchBuilder& end(std::size_t start)
    {
        const auto length = bytes_.size() - start - 2;
        bytes_[start] = static_cast<std::byte>((length >> 8) & 0xff);
        bytes_[start + 1] = static_cast<std::byte>(length & 0xff);
        return *this;
    }

    // Appends the low `width` bytes of `value`, big-endian.
    void put(std::uint64_t value, int width)
    {
        for (int shift = 8 * (width - 1); shift >= 0; shift -= 8) {
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        }
    }

    void put_char(char c)
    {
        bytes_.push_back(static_cast<std::byte>(c));
    }

    // Left-justified, space-padded alpha field (truncated to `width`).
    void put_alpha(std::string_view text, std::size_t width)
    {
        for (std::size_t i = 0; i < width; ++i) {
            put_char(i < text.size() ? text[i] : ' ');
        }
    }

    std::vector<std::byte> bytes_;
    std::uint16_t tracking_ = 0;
};

} // namespace simulator::itch_test
