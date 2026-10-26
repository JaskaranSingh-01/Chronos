#include "simulator/market/pipeline_source.hpp"

namespace simulator {

namespace {

// One turn of a wait on the other thread: a CPU pause for the first spins, then a yield to the scheduler.
void back_off(unsigned& spins) noexcept
{
    if (++spins < 64) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
    } else {
        std::this_thread::yield();
    }
}

} // namespace

PipelineSource::PipelineSource(MarketEventSource& source, std::size_t capacity)
    : source_(source), ring_(capacity), thread_([this] { produce(); })
{}

PipelineSource::~PipelineSource()
{
    stop();
    released_.store(true, std::memory_order_release);
    released_.notify_one();
    thread_.join();
}

void PipelineSource::produce() noexcept
{
    started_.wait(false, std::memory_order_acquire);
    read();
    done_.store(true, std::memory_order_release);
    done_.notify_one();
    released_.wait(false, std::memory_order_acquire);
}

void PipelineSource::read() noexcept
{
    Slot slot;
    while (state_.load(std::memory_order_acquire) != State::Stopped) {
        const SourceResult result = source_.next(slot.event);
        if (result != SourceResult::Event) {
            end_ = result;
            error_ = source_.error();
            end_position_ = source_.position();
            state_.store(State::Ended, std::memory_order_release);
            return;
        }
        slot.position = source_.position();
        for (unsigned spins = 0; !ring_.try_push(slot);) {
            if (state_.load(std::memory_order_acquire) == State::Stopped) {
                return;
            }
            back_off(spins);
        }
    }
}

void PipelineSource::start() noexcept
{
    started_.store(true, std::memory_order_release);
    started_.notify_one();
}

void PipelineSource::stop() noexcept
{
    state_.store(State::Stopped, std::memory_order_release);
    start();  // a thread never started still has to see Stopped
    done_.wait(false, std::memory_order_acquire);
}

// Reads the state before the ring: once Ended is seen, every event pushed before it is in the ring, so an empty ring
// is the end.
SourceResult PipelineSource::next(MarketEvent& event) noexcept
{
    Slot slot;
    for (unsigned spins = 0;; back_off(spins)) {
        const bool ended = state_.load(std::memory_order_acquire) == State::Ended;
        if (ring_.try_pop(slot)) {
            event = slot.event;
            position_ = slot.position;
            return SourceResult::Event;
        }
        if (ended) {
            position_ = end_position_;
            return end_;
        }
    }
}

std::uint64_t PipelineSource::position() const noexcept
{
    return position_;
}

SourceError PipelineSource::error() const noexcept
{
    return error_;
}

} // namespace simulator
