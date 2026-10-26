#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "simulator/types/symbol_id.hpp"

namespace simulator {

struct Instrument
{
    std::array<char, 8> venue{};           // MIC, NUL-padded, e.g. "XNAS"
    std::array<char, 32> native_symbol{};  // NUL-padded, e.g. "AAPL", "BTC-USD"
    std::uint8_t price_scale{0};           // decimal places: value = ticks x 10^-price_scale
    std::uint8_t quantity_scale{0};        // decimal places: 0 = whole shares

    // Minimum price increment in ticks, informational only. 0 = unspecified
    // (US equities step $0.01 at $1.00 and above, $0.0001 below).
    std::int64_t tick_size{1};

    bool operator==(const Instrument&) const = default;
};

enum class RegisterResult : std::uint8_t {
    Added,
    Existing,
    Conflict,
    Full
};

// Maps dense SymbolIds to instruments. Sources register an instrument before
// emitting its events; the caller keeps the registry alive for the whole run.
class InstrumentRegistry
{
private:
    std::unique_ptr<Instrument[]> instruments_;  // index = SymbolId
    std::size_t capacity_{0};
    std::size_t size_{0};

public:
    // Reserves `capacity` instruments; the only allocation.
    explicit InstrumentRegistry(std::size_t capacity);

    // Keyed on (venue, native_symbol): a repeat gives Existing, the same key with other
    // scales or tick size gives Conflict. Writes `id` only on Added and Existing.
    RegisterResult add(const Instrument& instrument, SymbolId& id) noexcept;

    // nullptr if unknown.
    const Instrument* find(SymbolId id) const noexcept
    {
        return id < size_ ? &instruments_[id] : nullptr;
    }

    std::size_t size() const noexcept
    {
        return size_;
    }

    std::size_t capacity() const noexcept
    {
        return capacity_;
    }
};

} // namespace simulator
