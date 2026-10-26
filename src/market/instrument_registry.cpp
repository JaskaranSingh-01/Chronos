#include "simulator/market/instrument_registry.hpp"

namespace simulator {

InstrumentRegistry::InstrumentRegistry(std::size_t capacity)
    : instruments_(std::make_unique<Instrument[]>(capacity)), capacity_(capacity)
{}


RegisterResult InstrumentRegistry::add(const Instrument& instrument, SymbolId& id) noexcept
{
    for (std::size_t i = 0; i < size_; ++i) {
        const Instrument& known = instruments_[i];

        if (known.venue != instrument.venue || known.native_symbol != instrument.native_symbol) {
            continue;
        }

        if (known != instrument) {
            return RegisterResult::Conflict;
        }

        id = static_cast<SymbolId>(i);
        return RegisterResult::Existing;
    }

    if (size_ == capacity_) {
        return RegisterResult::Full;
    }

    id = static_cast<SymbolId>(size_);
    instruments_[size_++] = instrument;
    return RegisterResult::Added;
}

} // namespace simulator
