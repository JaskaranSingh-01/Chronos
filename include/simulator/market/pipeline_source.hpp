#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "simulator/market/market_event_source.hpp"
#include "simulator/market/spsc_ring.hpp"

namespace simulator {

// `source` read on a thread of its own into a ring that next() reads on the replay's (spec §4): the decoder thread
// decodes, maps locates and registers instruments, and the replay's thread applies what it reads. Its thread and ring
// are made by the constructor, before the replay; start() lets the thread read, and stop(), however the replay ended,
// waits until it is done with `source`. The destructor ends and joins the thread, so the heap it frees as it exits is
// freed after the replay (spec §3.3).
//
// The end travels outside the ring, in `state_`. The producer, at the source's end or error, writes that end, then
// store-releases Ended; next() load-acquires the state before it reads the ring, so a ring empty after Ended is
// drained, and returns that end. stop() store-releases Stopped, which the producer checks before each push and while
// it waits on a full ring. Neither side waits on the other's join.
// ponytail: chronos-serve's sessions decode on their engine thread; give them a PipelineSource if one is decode-bound.
class PipelineSource final : public MarketEventSource
{
private:
    enum class State : std::uint8_t {
        Running,
        Ended,
        Stopped
    };

    // An event, and where its source read it: a failure on the replay's thread names the position a single thread
    // would.
    struct Slot
    {
        MarketEvent event{};
        std::uint64_t position{0};
    };

    MarketEventSource& source_;
    SpscRing<Slot> ring_;
    std::atomic<State> state_{State::Running};
    std::atomic<bool> started_{false};
    std::atomic<bool> done_{false};      // the decoder is done with the source
    std::atomic<bool> released_{false};  // the destructor lets the thread end
    // The source's end, the producer's to write until Ended:
    SourceResult end_{SourceResult::EndOfStream};
    SourceError error_{SourceError::None};
    std::uint64_t end_position_{0};
    alignas(128) std::uint64_t position_{0};  // the replay thread's, of the last event read: off state_'s line
    std::thread thread_;

    void produce() noexcept;
    void read() noexcept;

public:
    // `capacity`, the ring's, is a power of two.
    explicit PipelineSource(MarketEventSource& source, std::size_t capacity = 65'536);
    ~PipelineSource() override;

    PipelineSource(const PipelineSource&) = delete;
    PipelineSource& operator=(const PipelineSource&) = delete;

    void start() noexcept;
    void stop() noexcept;

    SourceResult next(MarketEvent& event) noexcept override;
    std::uint64_t position() const noexcept override;
    SourceError error() const noexcept override;

    std::size_t bytes() const noexcept
    {
        return ring_.bytes();
    }
};

} // namespace simulator
