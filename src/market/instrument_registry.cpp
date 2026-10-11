#include "simulator/market/instrument_registry.hpp"

namespace simulator {

InstrumentRegistry::InstrumentRegistry(std::size_t capacity)
    : capacity_(capacity)
{
    instruments_.reserve(capacity);
}


RegisterResult InstrumentRegistry::add(const Instrument& instrument, SymbolId& id) noexcept
{
    for (std::size_t i = 0; i < instruments_.size(); ++i) {
        const Instrument& known = instruments_[i];

        if (known.venue != instrument.venue ||
            known.native_symbol != instrument.native_symbol) {
            continue;
        }

        if (known != instrument) {
            return RegisterResult::Conflict;
        }

        id = static_cast<SymbolId>(i);
        return RegisterResult::Existing;
    }

    if (instruments_.size() == capacity_) {
        return RegisterResult::Full;
    }

    // Cannot allocate: the constructor reserved capacity_.
    id = static_cast<SymbolId>(instruments_.size());
    instruments_.push_back(instrument);
    return RegisterResult::Added;
}

} // namespace simulator
