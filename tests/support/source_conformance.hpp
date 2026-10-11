#pragma once

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "simulator/market/instrument_registry.hpp"
#include "simulator/market/market_event_source.hpp"

// The source conformance suite: the contract of market_event_source.hpp as
// checks that every MarketEventSource must pass.
//
// Each check is a plain function that drains a source and returns its first
// violation, or nullopt, so it can also be run against a deliberately broken
// source. SourceConformanceTest (tests/source_conformance_test.cpp) runs them
// all for every SourceCase.
//
// Adding a source: one INSTANTIATE_TEST_SUITE_P line in
// tests/source_conformance_test.cpp, plus one small synthetic data file under
// tests/data/conformance/ (never a slice of real data) unless the input is
// built in the test (SourceCase::data) or the events come from an in-test array.

namespace simulator::conformance {

using Violation = std::optional<std::string>;

using SourceFactory = std::unique_ptr<MarketEventSource> (*)(
    std::span<const std::byte> data, InstrumentRegistry& registry);

struct SourceCase
{
    std::string_view name;
    std::string_view data_file;             // under tests/data/conformance/; empty: no file
    std::string_view data;                  // in-memory good input, used when data_file is empty
    SourceFactory factory{nullptr};         // registers its instruments in `registry`
    std::string_view bad_input;             // must end in expected_error
    SourceError expected_error{SourceError::None};  // None: cannot fail, the error check is skipped
    std::uint64_t expected_position{0};
};

inline void PrintTo(const SourceCase& source_case, std::ostream* os)
{
    *os << source_case.name;
}

inline std::span<const std::byte> as_bytes(std::string_view text) noexcept
{
    return std::as_bytes(std::span{text.data(), text.size()});
}

inline std::string_view type_name(MarketEventType type) noexcept
{
    constexpr std::array<std::string_view, market_event_type_count> names{
        "Add", "Modify", "Replace", "Cancel", "Delete", "Execute", "Clear", "Trade"};
    const auto index = static_cast<std::size_t>(type);
    return index < names.size() ? names[index] : "unknown type";
}

inline std::string at(const MarketEventSource& source)
{
    return "position " + std::to_string(source.position()) + ": ";
}

// Calls on_event for every event until EndOfStream. The first violation is
// on_event's, an Error (the input is meant to be good) or an EndOfStream that
// is not sticky.
template <class OnEvent>
Violation for_each_event(MarketEventSource& source, OnEvent&& on_event)
{
    MarketEvent event;

    for (;;) {
        switch (source.next(event)) {
        case SourceResult::Event:
            if (Violation violation = on_event(event)) {
                return violation;
            }
            break;
        case SourceResult::EndOfStream:
            if (source.next(event) != SourceResult::EndOfStream) {
                return at(source) + "EndOfStream is not sticky";
            }
            return std::nullopt;
        case SourceResult::Error:
            return at(source) + "Error " +
                std::to_string(static_cast<int>(source.error())) + " on good input";
        }
    }
}

// Timestamps never go backwards.
inline Violation check_time(MarketEventSource& source)
{
    Timestamp last = std::numeric_limits<Timestamp>::min();

    return for_each_event(source, [&](const MarketEvent& event) -> Violation {
        if (event.timestamp < last) {
            return at(source) + "timestamp " + std::to_string(event.timestamp) +
                " is before " + std::to_string(last);
        }
        last = event.timestamp;
        return std::nullopt;
    });
}

// Every Modify, Replace, Cancel, Delete or Execute names an id that an earlier
// Add (or Replace's new_order_id) created for that symbol and that is still
// live. Live ids are never reused. Cancel and Execute sizes are <= the order's
// remaining size.
inline Violation check_references(MarketEventSource& source)
{
    // Remaining size of every live order, by (symbol, order_id).
    std::map<std::pair<SymbolId, OrderId>, std::uint64_t> live;

    return for_each_event(source, [&](const MarketEvent& event) -> Violation {
        const auto order = live.find({event.symbol, event.order_id});
        const std::uint64_t size = event.quantity.value();

        const auto fail = [&](std::string_view what, OrderId id) -> Violation {
            return at(source) + std::string{type_name(event.type)} + " " + std::string{what} +
                " id " + std::to_string(id) + " of symbol " + std::to_string(event.symbol);
        };

        switch (event.type) {
        case MarketEventType::Add:
            if (order != live.end()) {
                return fail("reuses live", event.order_id);
            }
            live.emplace(std::pair{event.symbol, event.order_id}, size);
            return std::nullopt;

        case MarketEventType::Modify:
            if (order == live.end()) {
                return fail("of unknown", event.order_id);
            }
            order->second = size;
            return std::nullopt;

        case MarketEventType::Replace:
            if (order == live.end()) {
                return fail("of unknown", event.order_id);
            }
            // Checked before the old order goes, so new == old is a reuse.
            if (live.contains({event.symbol, event.new_order_id})) {
                return fail("reuses live", event.new_order_id);
            }
            live.erase(order);
            live.emplace(std::pair{event.symbol, event.new_order_id}, size);
            return std::nullopt;

        case MarketEventType::Cancel:
        case MarketEventType::Execute:
            if (order == live.end()) {
                return fail("of unknown", event.order_id);
            }
            if (size > order->second) {
                return fail("of " + std::to_string(size) + " exceeds the " +
                    std::to_string(order->second) + " remaining of", event.order_id);
            }
            order->second -= size;
            if (order->second == 0) {
                live.erase(order);
            }
            return std::nullopt;

        case MarketEventType::Delete:
            if (order == live.end()) {
                return fail("of unknown", event.order_id);
            }
            live.erase(order);
            return std::nullopt;

        case MarketEventType::Clear:
            std::erase_if(live, [&](const auto& entry) {
                return entry.first.first == event.symbol;
            });
            return std::nullopt;

        case MarketEventType::Trade:
            return std::nullopt;
        }
        return at(source) + "unknown event type " +
            std::to_string(static_cast<int>(event.type));
    });
}

// The symbol is registered before its first event. Required prices and sizes
// are > 0, and Add, Modify and Replace prices are multiples of tick_size.
// tick_size 0 means unspecified (US equities step by price), so it is skipped.
inline Violation check_scales(MarketEventSource& source, const InstrumentRegistry& registry)
{
    return for_each_event(source, [&](const MarketEvent& event) -> Violation {
        const Instrument* instrument = registry.find(event.symbol);

        if (instrument == nullptr) {
            return at(source) + "symbol " + std::to_string(event.symbol) + " is not registered";
        }

        const MarketEventType type = event.type;
        const bool limit_price = type == MarketEventType::Add ||
            type == MarketEventType::Modify || type == MarketEventType::Replace;
        const bool needs_price = limit_price || type == MarketEventType::Trade;
        const bool needs_size = needs_price ||
            type == MarketEventType::Cancel || type == MarketEventType::Execute;

        if (needs_price && event.price.ticks <= 0) {
            return at(source) + std::string{type_name(type)} + " price " +
                std::to_string(event.price.ticks) + " is not > 0";
        }
        if (needs_size && event.quantity.is_zero()) {
            return at(source) + std::string{type_name(type)} + " size is 0";
        }
        if (limit_price && instrument->tick_size > 0 &&
            event.price.ticks % instrument->tick_size != 0) {
            return at(source) + "price " + std::to_string(event.price.ticks) +
                " is not a multiple of tick_size " + std::to_string(instrument->tick_size);
        }
        return std::nullopt;
    });
}

inline bool same_event(const MarketEvent& a, const MarketEvent& b) noexcept
{
    return a.timestamp == b.timestamp && a.order_id == b.order_id &&
        a.new_order_id == b.new_order_id && a.price == b.price &&
        a.quantity == b.quantity && a.symbol == b.symbol &&
        a.type == b.type && a.side == b.side;
}

// Two fresh sources over the same bytes and registry give identical events.
// Also fails when the good input gives no events, so no check passes vacuously.
inline Violation check_determinism(MarketEventSource& first, MarketEventSource& second)
{
    std::vector<MarketEvent> events;

    if (Violation violation = for_each_event(first, [&](const MarketEvent& event) -> Violation {
            events.push_back(event);
            return std::nullopt;
        })) {
        return violation;
    }
    if (events.empty()) {
        return "the good input gave no events";
    }

    std::size_t count = 0;

    if (Violation violation = for_each_event(second, [&](const MarketEvent& event) -> Violation {
            if (count == events.size() || !same_event(events[count], event)) {
                return at(second) + "event " + std::to_string(count + 1) +
                    " differs from the first read";
            }
            ++count;
            return std::nullopt;
        })) {
        return violation;
    }
    if (count != events.size()) {
        return "the second read gave " + std::to_string(count) +
            " events, the first " + std::to_string(events.size());
    }
    return std::nullopt;
}

// Bad input ends in a sticky Error with the expected error() and a non-zero,
// expected position().
inline Violation check_error(
    MarketEventSource& source, SourceError expected_error, std::uint64_t expected_position)
{
    MarketEvent event;
    SourceResult result;

    while ((result = source.next(event)) == SourceResult::Event) {
    }

    if (result != SourceResult::Error) {
        return at(source) + "bad input ended in EndOfStream, not Error";
    }

    const SourceError error = source.error();
    const std::uint64_t position = source.position();

    if (error != expected_error) {
        return at(source) + "error " + std::to_string(static_cast<int>(error)) +
            ", expected " + std::to_string(static_cast<int>(expected_error));
    }
    if (position == 0 || position != expected_position) {
        return at(source) + "expected position " + std::to_string(expected_position) +
            " (non-zero)";
    }
    if (source.next(event) != SourceResult::Error ||
        source.error() != error || source.position() != position) {
        return at(source) + "Error is not sticky";
    }
    return std::nullopt;
}

// Runs the checks above for one SourceCase. Each test gets a fresh registry.
class SourceConformanceTest : public ::testing::TestWithParam<SourceCase>
{
protected:
    void SetUp() override
    {
        const std::string_view file = GetParam().data_file;

        if (file.empty()) {
            data_ = GetParam().data;
            return;
        }

        const std::string path =
            std::string{CHRONOS_TEST_DATA_DIR} + "/conformance/" + std::string{file};
        std::ifstream in{path, std::ios::binary};

        ASSERT_TRUE(in) << "cannot open " << path;
        data_.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    }

    std::unique_ptr<MarketEventSource> make(std::string_view input)
    {
        return GetParam().factory(as_bytes(input), registry_);
    }

    std::string data_;
    InstrumentRegistry registry_{16};
};

} // namespace simulator::conformance
