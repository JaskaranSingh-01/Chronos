#pragma once

#include <cstdint>
#include <span>

#include "simulator/market/market_event_source.hpp"

namespace simulator {

// A MarketEventSource over events already in memory, for tests and synthetic
// benchmarks. It never fails. The caller keeps `events` alive and registers
// their symbols.
class VectorSource final : public MarketEventSource
{
private:
    std::span<const MarketEvent> events_;
    std::uint64_t position_{0};

public:
    explicit VectorSource(std::span<const MarketEvent> events) noexcept
        : events_(events) {}

    // Never Error; EndOfStream is sticky.
    SourceResult next(MarketEvent& event) noexcept override
    {
        if (position_ == events_.size()) {
            return SourceResult::EndOfStream;
        }
        event = events_[position_++];
        return SourceResult::Event;
    }

    // 1-based index of the last event; 0 before the first.
    std::uint64_t position() const noexcept override
    {
        return position_;
    }

    SourceError error() const noexcept override
    {
        return SourceError::None;
    }
};

} // namespace simulator
