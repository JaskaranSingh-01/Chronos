#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "simulator/types/symbol_id.hpp"

namespace simulator {

struct Instrument
{
    std::array<char, 8> venue{};           // MIC, NUL-padded, e.g. "XNAS"
    std::array<char, 32> native_symbol{};  // NUL-padded, e.g. "AAPL", "BTC-USD"
    std::uint8_t price_scale{0};           // decimal places: value = ticks x 10^-price_scale
    std::uint8_t quantity_scale{0};        // decimal places: 0 = whole shares

    // Minimum price increment in ticks; informational, the engine never enforces it.
    // 0 means unspecified: US equities step $0.01 at $1.00 or more and $0.0001
    // below (Reg NMS Rule 612), so no single increment fits every price.
    // The conformance suite checks prices are multiples of it only when it is > 0.
    std::int64_t tick_size{1};

    bool operator==(const Instrument&) const = default;
};

enum class RegisterResult : std::uint8_t {
    Added,
    Existing,
    Conflict,
    Full
};

// Maps a dense SymbolId to its instrument. The caller owns it and it outlives
// the run. Sources register their instruments before emitting their events.
class InstrumentRegistry
{
private:
    // ponytail: linear scan in add(); a run registers a handful of instruments.
    // Hash on (venue, native_symbol) if a source ever registers every ITCH R (~9k).
    std::vector<Instrument> instruments_;  // index = SymbolId
    std::size_t capacity_{0};

public:
    // Reserves `capacity` instruments; the only allocation.
    explicit InstrumentRegistry(std::size_t capacity);

    // Idempotent on (venue, native_symbol): the same instrument again gives
    // Existing and its id. The same key with different scales or tick size
    // gives Conflict; a new key past capacity gives Full. `id` is written only
    // on Added and Existing.
    RegisterResult add(const Instrument& instrument, SymbolId& id) noexcept;

    // nullptr if unknown.
    const Instrument* find(SymbolId id) const noexcept
    {
        return id < instruments_.size() ? &instruments_[id] : nullptr;
    }

    std::size_t size() const noexcept
    {
        return instruments_.size();
    }
};

} // namespace simulator
