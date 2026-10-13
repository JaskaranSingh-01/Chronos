#include "simulator/csv/csv_source.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace simulator {

namespace {

constexpr std::string_view header = "timestamp,event_type,order_id,side,price,quantity";
constexpr std::string_view header_with_new_order_id =
    "timestamp,event_type,order_id,side,price,quantity,new_order_id";

constexpr std::uint64_t int64_max =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::uint64_t uint64_max = std::numeric_limits<std::uint64_t>::max();

using Fields = std::array<std::string_view, 7>;

// Digits only, at most `max`. Any other character is InvalidNumber; a leading
// '-' or too large a value is NumberOutOfRange.
SourceError parse_number(std::string_view text, std::uint64_t max, std::uint64_t& value) noexcept
{
    const bool negative = text.starts_with('-');
    const char* first = text.data() + (negative ? 1 : 0);
    const char* last = text.data() + text.size();
    const auto [end, ec] = std::from_chars(first, last, value);

    if (ec == std::errc::invalid_argument || end != last) {
        return SourceError::InvalidNumber;
    }
    if (negative || ec == std::errc::result_out_of_range || value > max) {
        return SourceError::NumberOutOfRange;
    }
    return SourceError::None;
}

// An empty column is 0, or InvalidNumber when required.
SourceError field(
    std::string_view text, bool required, std::uint64_t max, std::uint64_t& value) noexcept
{
    if (text.empty()) {
        value = 0;
        return required ? SourceError::InvalidNumber : SourceError::None;
    }
    return parse_number(text, max, value);
}

// BUY or SELL; an empty column is Buy unless required.
SourceError side_field(std::string_view text, bool required, Side& side) noexcept
{
    side = Side::Buy;
    if (text == "BUY") {
        return SourceError::None;
    }
    if (text == "SELL") {
        side = Side::Sell;
        return SourceError::None;
    }
    return text.empty() && !required ? SourceError::None : SourceError::InvalidSide;
}

bool parse_type(std::string_view text, MarketEventType& type) noexcept
{
    using T = MarketEventType;
    constexpr std::array<std::pair<std::string_view, T>, 6> keywords{{
        {"ADD", T::Add}, {"CANCEL", T::Cancel}, {"DELETE", T::Delete},
        {"EXECUTE", T::Execute}, {"REPLACE", T::Replace}, {"TRADE", T::Trade}}};

    for (const auto& [keyword, value] : keywords) {
        if (text == keyword) {
            type = value;
            return true;
        }
    }
    return false;
}

// True only for exactly `count` (<= 7) comma-separated columns.
bool split(std::string_view line, std::size_t count, Fields& fields) noexcept
{
    if (static_cast<std::size_t>(std::count(line.begin(), line.end(), ',')) + 1 != count) {
        return false;
    }

    std::size_t start = 0;
    for (std::size_t column = 0; column < count; ++column) {
        const std::size_t comma = std::min(line.find(',', start), line.size());
        fields[column] = std::string_view{line.data() + start, comma - start};
        start = comma + 1;
    }
    return true;
}

// One data row, checked in column order. `event` is written only on success.
SourceError parse_row(
    std::string_view line, std::size_t columns, SymbolId symbol, MarketEvent& event) noexcept
{
    using T = MarketEventType;

    Fields f{};  // an absent 7th column stays empty
    if (!split(line, columns, f)) {
        return SourceError::InvalidRow;
    }

    std::uint64_t timestamp{0};
    if (const SourceError error = field(f[0], true, int64_max, timestamp);
        error != SourceError::None) {
        return error;
    }

    T type{};
    if (!parse_type(f[1], type)) {
        return SourceError::InvalidEventType;
    }

    std::uint64_t order_id{0};
    Side side{Side::Buy};
    std::uint64_t price{0};
    std::uint64_t quantity{0};
    std::uint64_t new_order_id{0};
    const SourceError errors[] = {
        field(f[2], type != T::Trade, uint64_max, order_id),
        side_field(f[3], type == T::Add || type == T::Trade, side),
        field(f[4], type == T::Add || type == T::Replace || type == T::Trade, int64_max, price),
        field(f[5], type != T::Delete, uint64_max, quantity),
        field(f[6], type == T::Replace, uint64_max, new_order_id)};

    for (const SourceError error : errors) {
        if (error != SourceError::None) {
            return error;
        }
    }

    event = MarketEvent{
        .timestamp = static_cast<Timestamp>(timestamp),
        .order_id = order_id,
        .new_order_id = type == T::Replace ? new_order_id : std::uint64_t{0},
        .price = Price{type == T::Cancel || type == T::Delete
                           ? std::int64_t{0}
                           : static_cast<std::int64_t>(price)},
        .quantity = Quantity{type == T::Delete ? std::uint64_t{0} : quantity},
        .symbol = symbol,
        .type = type,
        .side = type == T::Add || type == T::Trade ? side : Side::Buy};
    return SourceError::None;
}

} // namespace

SourceResult CsvSource::finish(SourceError error) noexcept
{
    done_ = true;
    error_ = error;
    return error == SourceError::None ? SourceResult::EndOfStream : SourceResult::Error;
}

SourceResult CsvSource::next(MarketEvent& event) noexcept
{
    while (!done_) {
        if (offset_ >= text_.size()) {
            return finish(columns_ == 0 ? SourceError::InvalidHeader : SourceError::None);
        }

        const std::size_t end = std::min(text_.find('\n', offset_), text_.size());
        std::string_view line{text_.data() + offset_, end - offset_};
        offset_ = end + 1;
        ++line_;

        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }

        if (columns_ == 0) {
            columns_ = line == header ? 6 : line == header_with_new_order_id ? 7 : 0;
            if (columns_ == 0) {
                return finish(SourceError::InvalidHeader);
            }
            continue;
        }

        if (const SourceError error = parse_row(line, columns_, symbol_, event);
            error != SourceError::None) {
            return finish(error);
        }
        return SourceResult::Event;
    }
    return error_ == SourceError::None ? SourceResult::EndOfStream : SourceResult::Error;
}

} // namespace simulator
