#include "simulator/market/market_replay.hpp"

namespace simulator {

ReplayReport MarketReplay::run(MarketEventSource& source, MarketBook& book,
                               const ReplayOptions& options) noexcept
{
    ReplayReport report;
    MarketEvent event;
    std::uint64_t until_check = options.check_invariants_every;

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

        const ApplyResult applied = book.apply(event);
        ++report.by_type[static_cast<std::size_t>(event.type)];
        ++report.by_result[static_cast<std::size_t>(applied)];

        if (applied != ApplyResult::Applied || until_check == 0 || --until_check != 0) {
            continue;
        }
        until_check = options.check_invariants_every;
        ++report.invariant_checks;

        if (!book.check_invariants(report.invariant_error,
                                   in_rth(event.timestamp, options.session_midnight))) {
            fail(ReplayFailure::InvariantViolation, to_string(report.invariant_error));
            break;
        }
    }

    return report;
}

std::string_view to_string(MarketEventType type) noexcept
{
    switch (type) {
    case MarketEventType::Add: return "Add";
    case MarketEventType::Modify: return "Modify";
    case MarketEventType::Replace: return "Replace";
    case MarketEventType::Cancel: return "Cancel";
    case MarketEventType::Delete: return "Delete";
    case MarketEventType::Execute: return "Execute";
    case MarketEventType::Clear: return "Clear";
    case MarketEventType::Trade: return "Trade";
    }
    return "?";
}

std::string_view to_string(ApplyResult result) noexcept
{
    switch (result) {
    case ApplyResult::Applied: return "Applied";
    case ApplyResult::WrongSymbol: return "WrongSymbol";
    case ApplyResult::UnknownOrder: return "UnknownOrder";
    case ApplyResult::DuplicateOrder: return "DuplicateOrder";
    case ApplyResult::ZeroQuantity: return "ZeroQuantity";
    case ApplyResult::ExceedsRemaining: return "ExceedsRemaining";
    case ApplyResult::InvalidReplace: return "InvalidReplace";
    case ApplyResult::PoolExhausted: return "PoolExhausted";
    case ApplyResult::Unsupported: return "Unsupported";
    }
    return "?";
}

std::string_view to_string(SourceError error) noexcept
{
    switch (error) {
    case SourceError::None: return "None";
    case SourceError::TruncatedMessage: return "TruncatedMessage";
    case SourceError::InvalidLength: return "InvalidLength";
    case SourceError::InvalidPayloadLength: return "InvalidPayloadLength";
    case SourceError::InvalidSide: return "InvalidSide";
    case SourceError::SymbolNotFound: return "SymbolNotFound";
    case SourceError::InstrumentRejected: return "InstrumentRejected";
    case SourceError::InvalidHeader: return "InvalidHeader";
    case SourceError::InvalidRow: return "InvalidRow";
    case SourceError::InvalidNumber: return "InvalidNumber";
    case SourceError::NumberOutOfRange: return "NumberOutOfRange";
    case SourceError::InvalidEventType: return "InvalidEventType";
    }
    return "?";
}

std::string_view to_string(InvariantError error) noexcept
{
    switch (error) {
    case InvariantError::None: return "None";
    case InvariantError::IndexWithoutLevel: return "IndexWithoutLevel";
    case InvariantError::LevelWithoutIndex: return "LevelWithoutIndex";
    case InvariantError::LevelTotalMismatch: return "LevelTotalMismatch";
    case InvariantError::EmptyLevel: return "EmptyLevel";
    case InvariantError::SequenceNotIncreasing: return "SequenceNotIncreasing";
    case InvariantError::InactiveOrderInBook: return "InactiveOrderInBook";
    case InvariantError::PoolCountMismatch: return "PoolCountMismatch";
    case InvariantError::CrossedInRth: return "CrossedInRth";
    }
    return "?";
}

} // namespace simulator
