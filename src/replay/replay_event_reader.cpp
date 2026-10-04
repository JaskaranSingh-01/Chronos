#include "simulator/replay/replay_event_reader.hpp"

#include <cstdint>
#include <sstream>
#include <string>

namespace simulator {
namespace {

bool parse_side(
    const std::string& value,
    Side& side
)
{
    if (value == "BUY") {
        side = Side::Buy;
        return true;
    }

    if (value == "SELL") {
        side = Side::Sell;
        return true;
    }

    return false;
}

bool parse_event_type(
    const std::string& value,
    ReplayEventType& type
)
{
    if (value == "ADD") {
        type = ReplayEventType::Add;
        return true;
    }

    if (value == "CANCEL") {
        type = ReplayEventType::Cancel;
        return true;
    }

    return false;
}

} // namespace

ReplayEventReader::ReplayEventReader(
    const std::string& filename
)
    : file_(filename)
{
    if (!file_.is_open()) {
        error_ = "Failed to open replay file: " + filename;
    }
}

ReplayReadResult ReplayEventReader::next(
    ReplayEvent& event
)
{
    if (!file_.is_open()) {
        return ReplayReadResult::Error;
    }

    std::string line;

    while (std::getline(file_, line)) {
        // Handle Windows CRLF files.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Skip empty lines.
        if (line.empty()) {
            continue;
        }

        // The first non-empty line must be the header.
        if (!header_read_) {
            header_read_ = true;

            if (line !=
                "timestamp,event_type,order_id,side,price,quantity")
            {
                error_ = "Invalid CSV header";
                return ReplayReadResult::Error;
            }

            continue;
        }

        if (parse_event(line, event)) {
            return ReplayReadResult::Event;
        }

        return ReplayReadResult::Error;
    }

    return ReplayReadResult::EndOfFile;
}

bool ReplayEventReader::parse_event(
    const std::string& line,
    ReplayEvent& event
)
{
    std::stringstream stream(line);

    std::string timestamp_string;
    std::string event_type_string;
    std::string order_id_string;
    std::string side_string;
    std::string price_string;
    std::string quantity_string;

    if (!std::getline(stream, timestamp_string, ',') ||
        !std::getline(stream, event_type_string, ',') ||
        !std::getline(stream, order_id_string, ',') ||
        !std::getline(stream, side_string, ',') ||
        !std::getline(stream, price_string, ',') ||
        !std::getline(stream, quantity_string, ','))
    {
        error_ = "Invalid CSV row: " + line;
        return false;
    }

    // Make sure there are no extra columns.
    std::string extra;

    if (std::getline(stream, extra, ',')) {
        error_ = "Too many columns in CSV row: " + line;
        return false;
    }

    try {
        const std::int64_t timestamp =
            std::stoll(timestamp_string);

        const std::uint64_t order_id =
            std::stoull(order_id_string);

        const std::int64_t price =
            std::stoll(price_string);

        const std::uint32_t quantity =
            static_cast<std::uint32_t>(
                std::stoul(quantity_string)
            );

        ReplayEventType event_type;

        if (!parse_event_type(
                event_type_string,
                event_type))
        {
            error_ =
                "Invalid event type: " +
                event_type_string;

            return false;
        }

        Side side;

        if (!parse_side(side_string, side)) {
            error_ =
                "Invalid side: " +
                side_string;

            return false;
        }

        event.timestamp = Timestamp{timestamp};
        event.type = event_type;
        event.order_id = OrderId{order_id};
        event.side = side;
        event.price = Price{price};
        event.quantity = Quantity{quantity};

        return true;
    }
    catch (...) {
        error_ = "Invalid numeric value in CSV row: " + line;
        return false;
    }
}

} // namespace simulator