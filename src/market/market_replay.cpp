#include "simulator/market/market_replay.hpp"

#include <algorithm>
#include <chrono>

#include "simulator/metrics/latency_histogram.hpp"

namespace simulator {

namespace {

// The loop both run()s share. `route(event)` gives the event's book, or nullptr
// (WrongSymbol); `count(event, result)` sees every result. `event` ends as the last read.
template <bool Timed, class Route, class Count>
void replay_loop(MarketEventSource& source, const ReplayOptions& options, ReplayReport& report, MarketEvent& event,
                 Route& route, Count& count) noexcept
{
    using clock = std::chrono::steady_clock;

    std::uint64_t until_check = options.check_invariants_every;
    std::uint64_t until_sample = options.latency_batch;
    clock::time_point batch_start = Timed ? clock::now() : clock::time_point{};

    auto fail = [&](ReplayFailure failure, std::string_view message) {
        report.failure = failure;
        report.failed_position = source.position();
        report.message = message;
    };

    for (;;) {
        const SourceResult result = source.next(event);

        if (result == SourceResult::EndOfStream) {
            break;
        }
        if (result == SourceResult::Error) {
            report.source_error = source.error();
            fail(ReplayFailure::SourceError, to_string(report.source_error));
            break;
        }

        if (report.events == 0) {
            report.first_timestamp = event.timestamp;
        } else if (event.timestamp < report.last_timestamp) {
            fail(ReplayFailure::OutOfOrderTimestamp, "timestamp earlier than the previous event's");
            break;
        }
        report.last_timestamp = event.timestamp;
        ++report.events;

        MarketBook* book = route(event);
        const ApplyResult applied = book != nullptr ? book->apply(event) : ApplyResult::WrongSymbol;
        ++report.by_type[static_cast<std::size_t>(event.type)];
        ++report.by_result[static_cast<std::size_t>(applied)];
        count(event, applied);

        if constexpr (Timed) {
            if (--until_sample == 0) {
                const clock::time_point now = clock::now();
                const auto batch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - batch_start);
                options.latency->record(static_cast<std::uint64_t>(batch_ns.count()) / options.latency_batch);
                batch_start = now;
                until_sample = options.latency_batch;
            }
        }

        if (applied != ApplyResult::Applied || until_check == 0 || --until_check != 0) {
            continue;
        }
        until_check = options.check_invariants_every;
        ++report.invariant_checks;

        if (!book->check_invariants(report.invariant_error, in_rth(event.timestamp, options.session_midnight))) {
            fail(ReplayFailure::InvariantViolation, to_string(report.invariant_error));
            break;
        }
    }
}

// Picks the timed loop only when latency sampling is on, so the other has no clock calls.
template <class Route, class Count>
void replay(MarketEventSource& source, const ReplayOptions& options, ReplayReport& report, MarketEvent& event,
            Route&& route, Count&& count) noexcept
{
    if (options.latency != nullptr && options.latency_batch != 0) {
        replay_loop<true>(source, options, report, event, route, count);
    } else {
        replay_loop<false>(source, options, report, event, route, count);
    }
}

} // namespace

ReplayReport MarketReplay::run(MarketEventSource& source, MarketBook& book, const ReplayOptions& options) noexcept
{
    ReplayReport report;
    MarketEvent event;
    replay(
        source, options, report, event, [&book](const MarketEvent&) { return &book; },
        [](const MarketEvent&, ApplyResult) {});
    return report;
}

BooksReplayReport MarketReplay::run(MarketEventSource& source, MarketBooks& books,
                                    const ReplayOptions& options) noexcept
{
    BooksReplayReport report;
    MarketEvent event;
    std::vector<SymbolReport>& symbols = report.symbols;

    replay(
        source, options, report.total, event,
        [&](const MarketEvent& e) {
            MarketBook* book = books.find(e.symbol);
            if (symbols.size() < books.size()) {
                symbols.resize(books.size());
            }
            return book;
        },
        [&](const MarketEvent& e, ApplyResult result) {
            if (e.symbol < symbols.size()) {
                ++symbols[e.symbol].events;
                ++symbols[e.symbol].by_result[static_cast<std::size_t>(result)];
            }
            report.peak_live_orders = std::max(report.peak_live_orders, books.pool().in_use());
        });

    books.sync();
    symbols.resize(books.size());
    for (SymbolId id = 0; id < books.size(); ++id) {
        symbols[id].peak_orders = books[id].peak_order_count();
        symbols[id].orders_left = books[id].order_count();
    }
    if (report.total.failure == ReplayFailure::InvariantViolation) {
        report.failed_symbol = event.symbol;
    }
    return report;
}

std::string_view to_string(MarketEventType type) noexcept
{
    switch (type) {
    case MarketEventType::Add:
        return "Add";
    case MarketEventType::Modify:
        return "Modify";
    case MarketEventType::Replace:
        return "Replace";
    case MarketEventType::Cancel:
        return "Cancel";
    case MarketEventType::Delete:
        return "Delete";
    case MarketEventType::Execute:
        return "Execute";
    case MarketEventType::Clear:
        return "Clear";
    case MarketEventType::Trade:
        return "Trade";
    case MarketEventType::Status:
        return "Status";
    }
    return "?";
}

std::string_view to_string(ApplyResult result) noexcept
{
    switch (result) {
    case ApplyResult::Applied:
        return "Applied";
    case ApplyResult::WrongSymbol:
        return "WrongSymbol";
    case ApplyResult::UnknownOrder:
        return "UnknownOrder";
    case ApplyResult::DuplicateOrder:
        return "DuplicateOrder";
    case ApplyResult::ZeroQuantity:
        return "ZeroQuantity";
    case ApplyResult::ExceedsRemaining:
        return "ExceedsRemaining";
    case ApplyResult::InvalidReplace:
        return "InvalidReplace";
    case ApplyResult::PoolExhausted:
        return "PoolExhausted";
    case ApplyResult::Unsupported:
        return "Unsupported";
    }
    return "?";
}

std::string_view to_string(SourceError error) noexcept
{
    switch (error) {
    case SourceError::None:
        return "None";
    case SourceError::TruncatedMessage:
        return "TruncatedMessage";
    case SourceError::InvalidLength:
        return "InvalidLength";
    case SourceError::InvalidPayloadLength:
        return "InvalidPayloadLength";
    case SourceError::InvalidSide:
        return "InvalidSide";
    case SourceError::SymbolNotFound:
        return "SymbolNotFound";
    case SourceError::InstrumentRejected:
        return "InstrumentRejected";
    case SourceError::InvalidHeader:
        return "InvalidHeader";
    case SourceError::InvalidRow:
        return "InvalidRow";
    case SourceError::InvalidNumber:
        return "InvalidNumber";
    case SourceError::NumberOutOfRange:
        return "NumberOutOfRange";
    case SourceError::InvalidEventType:
        return "InvalidEventType";
    case SourceError::InvalidTradingState:
        return "InvalidTradingState";
    case SourceError::InvalidPrintable:
        return "InvalidPrintable";
    }
    return "?";
}

std::string_view to_string(InvariantError error) noexcept
{
    switch (error) {
    case InvariantError::None:
        return "None";
    case InvariantError::IndexWithoutLevel:
        return "IndexWithoutLevel";
    case InvariantError::LevelWithoutIndex:
        return "LevelWithoutIndex";
    case InvariantError::LevelTotalMismatch:
        return "LevelTotalMismatch";
    case InvariantError::EmptyLevel:
        return "EmptyLevel";
    case InvariantError::SequenceNotIncreasing:
        return "SequenceNotIncreasing";
    case InvariantError::InactiveOrderInBook:
        return "InactiveOrderInBook";
    case InvariantError::PoolCountMismatch:
        return "PoolCountMismatch";
    case InvariantError::CrossedInRth:
        return "CrossedInRth";
    }
    return "?";
}

std::string_view to_string(TradingState state) noexcept
{
    switch (state) {
    case TradingState::Trading:
        return "Trading";
    case TradingState::Halted:
        return "Halted";
    case TradingState::Paused:
        return "Paused";
    case TradingState::QuotationOnly:
        return "QuotationOnly";
    }
    return "?";
}

} // namespace simulator
