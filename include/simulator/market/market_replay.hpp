#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_books.hpp"
#include "simulator/market/market_event_source.hpp"

namespace simulator {

class LatencyHistogram;

enum class ReplayFailure : std::uint8_t {
    None,
    SourceError,          // the source returned Error
    OutOfOrderTimestamp,  // an event earlier than the one before it; not applied
    InvariantViolation    // a periodic check_invariants() failed
};

struct ReplayOptions
{
    // 0 = never. Otherwise check_invariants() of the event's book after every
    // N-th applied event.
    std::uint64_t check_invariants_every{0};

    // session_midnight_ns() of the session: decides RTH (in_rth), where a
    // crossed book is a violation.
    Timestamp session_midnight{0};

    // When set, after every `latency_batch` events read (applied or not) the
    // batch's mean ns per event, by steady_clock, goes into it; a partial last
    // batch is dropped. Unset: the loop reads no clock.
    LatencyHistogram* latency{nullptr};
    std::uint64_t latency_batch{1024};
};

struct ReplayReport
{
    std::uint64_t events{0};  // read from the source, accepted or not
    std::array<std::uint64_t, market_event_type_count> by_type{};  // index = MarketEventType
    std::array<std::uint64_t, apply_result_count> by_result{};     // index = ApplyResult
    std::uint64_t invariant_checks{0};
    Timestamp first_timestamp{0};  // of the events handed to apply(); 0 if none
    Timestamp last_timestamp{0};

    // The first fatal failure; the run stops there.
    ReplayFailure failure{ReplayFailure::None};
    SourceError source_error{SourceError::None};
    InvariantError invariant_error{InvariantError::None};
    std::uint64_t failed_position{0};  // MarketEventSource::position() at the failure
    std::string_view message;          // e.g. "TruncatedMessage"; empty on success

    std::uint64_t applied() const noexcept
    {
        return by_result[static_cast<std::size_t>(ApplyResult::Applied)];
    }

    std::uint64_t rejected() const noexcept
    {
        return events - applied();
    }

    bool succeeded() const noexcept
    {
        return failure == ReplayFailure::None;
    }

    bool operator==(const ReplayReport&) const = default;
};

// One symbol's share of a multi-book run.
struct SymbolReport
{
    std::uint64_t events{0};
    std::array<std::uint64_t, apply_result_count> by_result{};  // index = ApplyResult
    std::size_t peak_orders{0};  // MarketBook::peak_order_count() at the end
    std::size_t orders_left{0};  // MarketBook::order_count() at the end

    std::uint64_t applied() const noexcept
    {
        return by_result[static_cast<std::size_t>(ApplyResult::Applied)];
    }

    std::uint64_t rejected() const noexcept
    {
        return events - applied();
    }

    bool operator==(const SymbolReport&) const = default;
};

struct BooksReplayReport
{
    // Counted as the single-book run counts; an event whose symbol has no
    // book (not registered) is WrongSymbol here and in no SymbolReport.
    ReplayReport total;
    std::vector<SymbolReport> symbols;  // index = SymbolId, one per book
    std::size_t peak_live_orders{0};    // max OrderPool::in_use(), all books together
    SymbolId failed_symbol{0};          // the book of an InvariantViolation

    bool operator==(const BooksReplayReport&) const = default;
};

// The M1 market-only loop: pulls events from a source into a book until
// EndOfStream or the first fatal failure. Rejected events are counted, not
// fatal. Deterministic and untimed: callers time run() from outside.
struct MarketReplay
{
    static ReplayReport run(MarketEventSource& source, MarketBook& book,
                            const ReplayOptions& options) noexcept;

    // Every event to the book of its symbol. At the end every registered
    // instrument has a book. Allocates only when a new symbol gets its book.
    static BooksReplayReport run(MarketEventSource& source, MarketBooks& books,
                                 const ReplayOptions& options) noexcept;
};

// Enumerator names, e.g. "UnknownOrder", for reports.
std::string_view to_string(MarketEventType type) noexcept;
std::string_view to_string(ApplyResult result) noexcept;
std::string_view to_string(SourceError error) noexcept;
std::string_view to_string(InvariantError error) noexcept;

} // namespace simulator
