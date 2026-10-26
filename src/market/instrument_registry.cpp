#include "simulator/market/instrument_registry.hpp"

namespace simulator {

InstrumentRegistry::InstrumentRegistry(std::size_t capacity)
    : instruments_(std::make_unique<Instrument[]>(capacity)), capacity_(capacity)
{}


RegisterResult InstrumentRegistry::add(const Instrument& instrument, SymbolId& id) noexcept
{
    // Every access is atomic while size() may read on another thread; relaxed, as this thread is the one writer.
    const std::size_t size = std::atomic_ref<std::size_t>{size_}.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < size; ++i) {
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

    if (size == capacity_) {
        return RegisterResult::Full;
    }

    id = static_cast<SymbolId>(size);
    instruments_[size] = instrument;
    std::atomic_ref<std::size_t>{size_}.store(size + 1, std::memory_order_release);
    return RegisterResult::Added;
}

} // namespace simulator
