#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "simulator/cli/replay_args.hpp"
#include "simulator/engine/event_loop.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_books.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/strategies/imbalance_taker.hpp"
#include "simulator/strategies/market_maker.hpp"

namespace {

using namespace simulator;

// ticks x 10^-scale as a decimal, e.g. 2'905'500 at scale 4 -> "290.5500".
std::string dollars(std::int64_t ticks, unsigned scale)
{
    std::uint64_t unit = 1;
    for (unsigned i = 0; i < scale; ++i) {
        unit *= 10;
    }
    const std::uint64_t magnitude = ticks < 0 ? 0 - static_cast<std::uint64_t>(ticks)
                                              : static_cast<std::uint64_t>(ticks);
    char text[32];
    const int written = std::snprintf(text, sizeof text, "%s%" PRIu64, ticks < 0 ? "-" : "", magnitude / unit);
    if (scale != 0) {
        std::snprintf(text + written, sizeof text - static_cast<std::size_t>(written), ".%0*" PRIu64,
                      static_cast<int>(scale), magnitude % unit);
    }
    return text;
}

// Money in notional units as "$1.2345" or "-$1.2345"; scale = price_scale + quantity_scale.
std::string usd(std::int64_t units, unsigned scale)
{
    const std::string text = dollars(units, scale);
    return text[0] == '-' ? "-$" + text.substr(1) : "$" + text;
}

// Exchange-local time of day, HH:MM:SS.nnnnnnnnn.
std::string clock_time(Timestamp timestamp, Timestamp session_midnight)
{
    const Timestamp ns = timestamp - session_midnight;
    constexpr Timestamp second = 1'000'000'000;
    char text[32];
    std::snprintf(text, sizeof text, "%02lld:%02lld:%02lld.%09lld",
                  static_cast<long long>(ns / (3600 * second)),
                  static_cast<long long>(ns / (60 * second) % 60),
                  static_cast<long long>(ns / second % 60), static_cast<long long>(ns % second));
    return text;
}

void print_levels(const char* side, const std::vector<LevelInfo>& levels, unsigned scale)
{
    for (const LevelInfo& level : levels) {
        std::printf("  %s %14s %12" PRIu64 " %8u\n", side, dollars(level.price.ticks, scale).c_str(),
                    level.quantity, level.order_count);
    }
}

void print_book(const MarketBook& book, std::size_t depth, unsigned scale)
{
    std::vector<LevelInfo> asks(depth);
    std::vector<LevelInfo> bids(depth);
    asks.resize(book.depth(Side::Sell, asks));
    bids.resize(book.depth(Side::Buy, bids));
    std::reverse(asks.begin(), asks.end());  // a ladder: worst ask on top, best bid below best ask

    std::printf("\nBook, top %zu levels per side\n", depth);
    std::printf("  side %14s %12s %8s\n", "price", "size", "orders");
    print_levels("ask ", asks, scale);
    print_levels("bid ", bids, scale);

    LevelInfo bid;
    LevelInfo ask;
    const std::string spread = book.best(Side::Buy, bid) && book.best(Side::Sell, ask)
        ? dollars(ask.price.ticks - bid.price.ticks, scale)
        : std::string{"n/a (a side is empty)"};
    std::printf("  spread %s, orders %zu, peak orders %zu, crossed %s, locked %s\n", spread.c_str(),
                book.order_count(), book.peak_order_count(), book.is_crossed() ? "yes" : "no",
                book.is_locked() ? "yes" : "no");
}

// The run header both commands print.
void print_config(const char* command, const ReplayArgs& args, const MappedFile& file)
{
    const auto offset = args.utc_offset.count();

    std::printf("Chronos %s\n", command);
    std::printf("  file             %.*s (%zu bytes)\n", static_cast<int>(args.file.size()),
                args.file.data(), file.bytes().size());
    if (args.all_symbols) {
        std::printf("  symbols          all\n");
    } else if (!args.symbols.empty()) {
        std::printf("  symbols         ");
        for (const std::string_view symbol : args.symbols) {
            std::printf(" %.*s", static_cast<int>(symbol.size()), symbol.data());
        }
        std::printf("\n");
    } else {
        std::printf("  symbol           %.*s\n", static_cast<int>(args.symbol.size()), args.symbol.data());
    }
    std::printf("  session          %04d-%02u-%02u, UTC%c%02lld:%02lld\n",
                static_cast<int>(args.date.year()), static_cast<unsigned>(args.date.month()),
                static_cast<unsigned>(args.date.day()), offset < 0 ? '-' : '+',
                static_cast<long long>(std::abs(offset) / 60), static_cast<long long>(std::abs(offset) % 60));
    std::printf("  truncated tail   %s\n", args.allow_truncated_tail ? "allowed" : "fatal");
    if (args.check_invariants_every == 0) {
        std::printf("  invariant checks at the end only\n");
    } else {
        std::printf("  invariant checks every %" PRIu64 " applied events, and at the end\n",
                    args.check_invariants_every);
    }
    std::printf("  capacity         %zu orders%s\n", args.capacity, args.symbol.empty() ? ", shared by every book" : "");
}

// "Applied N, rejected M" (after `indent`) and the rejections by reason.
void print_applied(const ReplayReport& report, const char* indent)
{
    std::printf("%sApplied %" PRIu64 ", rejected %" PRIu64 "\n", indent, report.applied(), report.rejected());
    for (std::size_t result = 1; result < apply_result_count; ++result) {
        if (report.by_result[result] != 0) {
            const std::string_view name = to_string(static_cast<ApplyResult>(result));
            std::printf("%s  %-16.*s %" PRIu64 "\n", indent, static_cast<int>(name.size()), name.data(),
                        report.by_result[result]);
        }
    }
}

void print_failure(const ReplayReport& report, const ReplayArgs& args)
{
    if (report.succeeded()) {
        return;
    }
    std::printf("\nFAILED: %.*s at position %" PRIu64 " (byte offset)\n",
                static_cast<int>(report.message.size()), report.message.data(), report.failed_position);
    if (report.source_error == SourceError::TruncatedMessage) {
        std::printf("  hint: the file ends inside a message (a slice cut at a byte count?);"
                    " pass --allow-truncated-tail to stop there instead\n");
    } else if (report.source_error == SourceError::SymbolNotFound) {
        std::printf("  hint: no stock directory (R) message for %.*s in this file\n",
                    static_cast<int>(args.symbol.size()), args.symbol.data());
    }
}

// The book, the end-of-run invariant check and the digest; false if the check failed.
bool print_final_book(const MarketBook& book, const ReplayReport& report, std::size_t depth, unsigned scale,
                      Timestamp midnight)
{
    print_book(book, depth, scale);

    InvariantError invariant_error{InvariantError::None};
    const bool invariants = book.check_invariants(invariant_error, in_rth(report.last_timestamp, midnight));
    const std::string_view invariant_name = to_string(invariant_error);
    std::printf("\nInvariants at end: %s%.*s (%" PRIu64 " periodic checks passed)\n",
                invariants ? "ok" : "FAILED ", invariants ? 0 : static_cast<int>(invariant_name.size()),
                invariant_name.data(), report.invariant_checks);
    std::printf("Digest            %016" PRIx64 "\n", book.digest());
    return invariants;
}

int replay(const ReplayArgs& args)
{
    MappedFile file;
    if (!file.open(std::string{args.file}.c_str())) {
        std::fprintf(stderr, "error: %s\n", file.error());
        return 2;
    }

    const ItchSourceConfig config{.ticker = args.symbol, .session_date = args.date,
                                  .utc_offset = args.utc_offset,
                                  .allow_truncated_tail = args.allow_truncated_tail};
    const Timestamp midnight = session_midnight_ns(args.date, args.utc_offset);

    print_config("replay", args, file);

    InstrumentRegistry registry{1};
    ItchSource source{file.bytes(), config, registry};
    MarketBook book{0, args.capacity};  // the registry's first id: ItchSource registers the only instrument

    const auto start = std::chrono::steady_clock::now();
    const ReplayReport report = MarketReplay::run(
        source, book, {.check_invariants_every = args.check_invariants_every, .session_midnight = midnight});
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    const ItchSourceStatistics& stats = source.statistics();
    std::printf("\nSource\n");
    std::printf("  messages %" PRIu64 ": %" PRIu64 " other symbols, %" PRIu64 " unused types, %" PRIu64
                " events; locate %u\n",
                stats.messages, stats.filtered, stats.skipped, stats.events, source.locate());
    std::printf("  truncated tail   %s\n", source.truncated_tail() ? "yes (last partial message ignored)" : "no");

    std::printf("\nEvents %" PRIu64, report.events);
    if (report.events != 0) {
        std::printf(", %s to %s local", clock_time(report.first_timestamp, midnight).c_str(),
                    clock_time(report.last_timestamp, midnight).c_str());
    }
    std::printf("\n");
    for (std::size_t type = 0; type < market_event_type_count; ++type) {
        const std::string_view name = to_string(static_cast<MarketEventType>(type));
        std::printf("  %-16.*s %" PRIu64 "\n", static_cast<int>(name.size()), name.data(), report.by_type[type]);
    }

    std::printf("\n");
    print_applied(report, "");
    print_failure(report, args);

    const bool invariants =
        print_final_book(book, report, args.depth, source.instrument().price_scale, midnight);

    const double seconds = elapsed.count();
    std::printf("Elapsed           %.1f ms, %.0f events/s, %.0f messages/s, %.1f MB/s\n", seconds * 1e3,
                static_cast<double>(report.events) / seconds, static_cast<double>(stats.messages) / seconds,
                static_cast<double>(file.bytes().size()) / 1e6 / seconds);

    return report.succeeded() && invariants ? 0 : 1;
}

// The NUL-padded native symbol without its padding.
std::string_view name(const Instrument& instrument)
{
    const auto& symbol = instrument.native_symbol;
    return {symbol.data(), static_cast<std::size_t>(std::find(symbol.begin(), symbol.end(), '\0') - symbol.begin())};
}

void print_symbol_row(std::string_view symbol, const SymbolReport& s)
{
    std::printf("  %-8.*s %12" PRIu64 " %12" PRIu64 " %10" PRIu64 " %12zu %12zu", static_cast<int>(symbol.size()),
                symbol.data(), s.events, s.applied(), s.rejected(), s.peak_orders, s.orders_left);
    for (std::size_t result = 1; result < apply_result_count; ++result) {
        if (s.by_result[result] != 0) {
            const std::string_view reason = to_string(static_cast<ApplyResult>(result));
            std::printf("  %.*s %" PRIu64, static_cast<int>(reason.size()), reason.data(), s.by_result[result]);
        }
    }
    std::printf("\n");
}

// --symbols or --all: one pass over the file, every symbol into its own book,
// all books on one shared OrderPool of --capacity slots.
int replay_books(const ReplayArgs& args)
{
    MappedFile file;
    if (!file.open(std::string{args.file}.c_str())) {
        std::fprintf(stderr, "error: %s\n", file.error());
        return 2;
    }

    const ItchSourceConfig config{.tickers = args.symbols, .all_tickers = args.all_symbols,
                                  .session_date = args.date, .utc_offset = args.utc_offset,
                                  .allow_truncated_tail = args.allow_truncated_tail};
    const Timestamp midnight = session_midnight_ns(args.date, args.utc_offset);

    print_config("replay", args, file);

    // ITCH locates are 16-bit, so a day has at most 65,536 instruments.
    InstrumentRegistry registry{args.all_symbols ? std::size_t{1} << 16 : args.symbols.size()};
    ItchSource source{file.bytes(), config, registry};
    OrderPool pool{args.capacity};
    MarketBooks books{registry, pool};

    const auto start = std::chrono::steady_clock::now();
    const BooksReplayReport report = MarketReplay::run(
        source, books, {.check_invariants_every = args.check_invariants_every, .session_midnight = midnight});
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    const ReplayReport& total = report.total;

    const ItchSourceStatistics& stats = source.statistics();
    std::printf("\nSource\n");
    std::printf("  messages %" PRIu64 ": %" PRIu64 " other symbols, %" PRIu64 " unused types, %" PRIu64 " events\n",
                stats.messages, stats.filtered, stats.skipped, stats.events);
    std::printf("  truncated tail   %s\n", source.truncated_tail() ? "yes (last partial message ignored)" : "no");
    for (const std::string_view symbol : args.symbols) {
        bool found = false;
        for (SymbolId id = 0; id < registry.size() && !found; ++id) {
            found = name(*registry.find(id)) == symbol;
        }
        if (!found) {
            std::printf("  not found        %.*s (no stock directory (R) message)\n", static_cast<int>(symbol.size()),
                        symbol.data());
        }
    }

    std::printf("\nEvents %" PRIu64, total.events);
    if (total.events != 0) {
        std::printf(", %s to %s local", clock_time(total.first_timestamp, midnight).c_str(),
                    clock_time(total.last_timestamp, midnight).c_str());
    }
    std::printf("\n");
    for (std::size_t type = 0; type < market_event_type_count; ++type) {
        const std::string_view type_name = to_string(static_cast<MarketEventType>(type));
        std::printf("  %-16.*s %" PRIu64 "\n", static_cast<int>(type_name.size()), type_name.data(),
                    total.by_type[type]);
    }

    std::printf("\n");
    print_applied(total, "");
    print_failure(total, args);
    if (total.failure == ReplayFailure::InvariantViolation) {
        const std::string_view failed = name(*registry.find(report.failed_symbol));
        std::printf("  in symbol %.*s\n", static_cast<int>(failed.size()), failed.data());
    }

    std::size_t non_empty = 0;
    for (const SymbolReport& symbol : report.symbols) {
        non_empty += symbol.orders_left != 0;
    }
    std::printf("\nBooks %zu: %zu empty at the end, %zu not empty\n", books.size(), books.size() - non_empty,
                non_empty);

    // --all: the 10 busiest, then others with rejections or orders left, in
    // SymbolId order: at most 40, since a file cut before 20:00 leaves most books non-empty.
    constexpr std::size_t max_others = 40;
    std::vector<SymbolId> rows(books.size());
    std::iota(rows.begin(), rows.end(), SymbolId{0});
    std::size_t hidden = 0;
    if (args.all_symbols) {
        std::stable_sort(rows.begin(), rows.end(), [&](SymbolId a, SymbolId b) {
            return report.symbols[a].events > report.symbols[b].events;
        });
        const std::size_t top = std::min<std::size_t>(10, rows.size());
        std::vector<SymbolId> others(rows.begin() + static_cast<std::ptrdiff_t>(top), rows.end());
        std::sort(others.begin(), others.end());
        rows.resize(top);
        for (const SymbolId id : others) {
            if (report.symbols[id].rejected() != 0 || report.symbols[id].orders_left != 0) {
                rows.size() < top + max_others ? rows.push_back(id) : void(++hidden);
            }
        }
        std::printf("Per symbol: the 10 with most events, then others with rejections or orders left\n");
    } else {
        std::printf("Per symbol\n");
    }
    std::printf("  %-8s %12s %12s %10s %12s %12s\n", "symbol", "events", "applied", "rejected", "peak orders",
                "orders left");
    for (const SymbolId id : rows) {
        print_symbol_row(name(*registry.find(id)), report.symbols[id]);
    }
    if (hidden != 0) {
        std::printf("  ... and %zu more with rejections or orders left\n", hidden);
    }

    std::printf("\nLive orders       peak %zu, %zu at the end, all books together (capacity %zu)\n",
                report.peak_live_orders, pool.in_use(), pool.capacity());

    // Every book's structure first, so one crossed book cannot hide another
    // problem; then every book crossed in RTH, not only the first.
    InvariantError invariant_error{InvariantError::None};
    SymbolId invariant_symbol{0};
    const bool structure = books.check_invariants(invariant_error, invariant_symbol);
    std::vector<SymbolId> crossed;
    if (in_rth(total.last_timestamp, midnight)) {
        for (SymbolId id = 0; id < books.size(); ++id) {
            if (books[id].is_crossed()) {
                crossed.push_back(id);
            }
        }
    }

    if (!structure) {
        const std::string_view error_name = to_string(invariant_error);
        const std::string_view failed =
            invariant_symbol < books.size() ? name(*registry.find(invariant_symbol)) : "(the shared pool)";
        std::printf("Invariants at end: FAILED %.*s in %.*s\n", static_cast<int>(error_name.size()),
                    error_name.data(), static_cast<int>(failed.size()), failed.data());
    } else if (!crossed.empty()) {
        std::printf("Invariants at end: FAILED CrossedInRth in %zu books:", crossed.size());
        for (std::size_t i = 0; i < crossed.size() && i < 20; ++i) {
            const std::string_view symbol = name(*registry.find(crossed[i]));
            std::printf(" %.*s", static_cast<int>(symbol.size()), symbol.data());
        }
        std::printf("%s; every other invariant ok in all %zu books\n", crossed.size() > 20 ? " ..." : "",
                    books.size());
    } else {
        std::printf("Invariants at end: ok, all %zu books\n", books.size());
    }
    std::printf("                   %" PRIu64 " periodic checks passed\n", total.invariant_checks);
    const bool invariants = structure && crossed.empty();
    std::printf("Digest (combined) %016" PRIx64 "\n", books.digest());

    const double seconds = elapsed.count();
    std::printf("Elapsed           %.2f s, %.0f events/s, %.0f messages/s, %.1f MB/s\n", seconds,
                static_cast<double>(total.events) / seconds, static_cast<double>(stats.messages) / seconds,
                static_cast<double>(file.bytes().size()) / 1e6 / seconds);

    return total.succeeded() && invariants ? 0 : 1;
}

void print_strategy(const ImbalanceTakerStats& stats)
{
    std::printf("\nStrategy\n");
    std::printf("  orders sent      %" PRIu64 "\n", stats.sent);
    std::printf("  risk rejects     %" PRIu64 "\n", stats.risk_rejects);
    std::printf("  fills            %" PRIu64 "\n", stats.fills);
    std::printf("  expired          %" PRIu64 "\n", stats.expired);
    std::printf("  bought           %" PRIu64 " shares\n", stats.bought);
    std::printf("  sold             %" PRIu64 " shares\n", stats.sold);
}

void print_strategy(const MarketMakerStats& stats)
{
    std::printf("\nStrategy\n");
    std::printf("  quotes sent      %" PRIu64 "\n", stats.quotes);
    std::printf("  risk rejects     %" PRIu64 "\n", stats.risk_rejects);
    std::printf("  cancels sent     %" PRIu64 "\n", stats.cancels);
    std::printf("  fills            %" PRIu64 "\n", stats.fills);
    std::printf("  rejects          %" PRIu64 " (would cross)\n", stats.rejects);
    std::printf("  bought           %" PRIu64 " shares\n", stats.bought);
    std::printf("  sold             %" PRIu64 " shares\n", stats.sold);
}

void print_execution(const ExecutionStatistics& stats)
{
    std::printf("\nExecution\n");
    std::printf("  maker fills      %" PRIu64 " (%" PRIu64 " shares)\n", stats.maker_fills, stats.maker_quantity);
    std::printf("  taker fills      %" PRIu64 " (%" PRIu64 " shares)\n", stats.taker_fills, stats.taker_quantity);
    if (stats.accepted_quantity != 0) {
        std::printf("  fill rate        %.2f%% of %" PRIu64 " shares quoted\n",
                    100.0 * static_cast<double>(stats.maker_quantity) / static_cast<double>(stats.accepted_quantity),
                    stats.accepted_quantity);
    }
    std::printf("  cancels          %" PRIu64 " done, %" PRIu64 " rejected (too late)\n", stats.cancelled,
                stats.cancel_rejects);
    std::printf("  would cross      %" PRIu64 " rejected\n", stats.would_cross_rejects);
    std::printf("  FIFO anomalies   %" PRIu64 "\n", stats.fifo_anomalies);
}

// Maker fills / FIFO anomalies by rule, historical message and session phase.
void print_attribution(const ExecutionStatistics& stats)
{
    static constexpr const char* causes[fill_cause_count] = {"(a) E", "(a) C", "(d) E", "(d) C", "(c) Add",
                                                             "(c) Replace"};
    std::printf("\nFill attribution (maker fills/FIFO anomalies; crosses = first second of 09:30, 16:00)\n");
    std::printf("  %-12s %13s %13s %13s %13s %13s\n", "rule, msg", "pre-open", "open cross", "regular",
                "close cross", "post-close");
    for (std::size_t cause = 0; cause < fill_cause_count; ++cause) {
        std::printf("  %-12s", causes[cause]);
        for (std::size_t phase = 0; phase < session_phase_count; ++phase) {
            char cell[32];
            std::snprintf(cell, sizeof cell, "%" PRIu64 "/%" PRIu64, stats.fills_by[cause][phase],
                          stats.anomalies_by[cause][phase]);
            std::printf(" %13s", cell);
        }
        std::printf("\n");
    }
}

// The first FIFO anomalies with their context, on stderr.
void print_anomaly_samples(const ExecutionSimulator& execution, Timestamp midnight, unsigned scale)
{
    for (const FifoAnomaly& a : execution.fifo_anomaly_samples()) {
        const ShadowOrder& o = a.order;
        std::fprintf(stderr,
                     "FIFO anomaly %s: %c id %" PRIu64 " x%" PRIu64 " exec price %s | h %s %s seq %" PRIu64
                     " remaining %" PRIu64 " | ours %" PRIu64 " %s %s anchor %" PRIu64 " ahead %" PRIu64
                     " remaining %" PRIu64 " | level %" PRIu64 "\n",
                     clock_time(a.event.timestamp, midnight).c_str(), a.event.price.ticks != 0 ? 'C' : 'E',
                     a.event.order_id, a.event.quantity.value(), dollars(a.event.price.ticks, scale).c_str(),
                     a.h_side == Side::Buy ? "buy" : "sell", dollars(a.h_price.ticks, scale).c_str(), a.h_sequence,
                     a.h_remaining, o.id, o.side == Side::Buy ? "buy" : "sell", dollars(o.price.ticks, scale).c_str(),
                     o.anchor_sequence, o.ahead_quantity, o.remaining, a.level_quantity);
    }
}

// MarketReplay plus a toy strategy through EventLoop: our orders filled
// against the replayed book (never changing it), with latency, risk and fees.
// Money prints in the instrument's currency.
int backtest(const ReplayArgs& args)
{
    MappedFile file;
    if (!file.open(std::string{args.file}.c_str())) {
        std::fprintf(stderr, "error: %s\n", file.error());
        return 2;
    }

    const BacktestArgs& b = args.backtest;
    const bool mm = b.strategy == StrategyKind::MarketMaker;
    const ItchSourceConfig config{.ticker = args.symbol, .session_date = args.date,
                                  .utc_offset = args.utc_offset,
                                  .allow_truncated_tail = args.allow_truncated_tail};
    const Timestamp midnight = session_midnight_ns(args.date, args.utc_offset);
    const LatencyConfig latency{.order_latency = static_cast<Timestamp>(b.order_latency_us) * 1'000,
                                .report_latency = static_cast<Timestamp>(b.report_latency_us) * 1'000};
    const ImbalanceTakerParams imbalance_params{.threshold = b.threshold, .clip = b.clip,
                                                .max_position = b.max_position,
                                                .cooldown_ns = static_cast<Timestamp>(b.cooldown_ms) * 1'000'000};
    const MarketMakerParams mm_params{.quote_size = b.quote_size, .max_position = b.max_position,
                                      .requote_cooldown_ns = static_cast<Timestamp>(b.requote_ms) * 1'000'000};
    // The strategy's own position limit, enforced again pre-trade.
    const RiskLimits limits{.max_position = b.max_position, .max_order_quantity = Quantity{mm ? b.quote_size : b.clip},
                            .max_open_orders = b.max_open_orders != 0 ? b.max_open_orders
                                                                      : default_max_open_orders(b.strategy)};
    const FeeSchedule fees{.maker_fee_per_share = b.maker_fee_ticks,
                           .taker_fee_per_share = static_cast<std::int64_t>(b.taker_fee_ticks)};

    InstrumentRegistry registry{1};
    ItchSource source{file.bytes(), config, registry};
    MarketBook book{0, args.capacity};  // the registry's first id: ItchSource registers the only instrument
    const Instrument& instrument = source.instrument();
    const unsigned price_scale = instrument.price_scale;
    const unsigned money_scale = instrument.price_scale + instrument.quantity_scale;

    print_config("backtest", args, file);
    std::printf("  latency          order %" PRIu64 " us, report %" PRIu64 " us\n", b.order_latency_us,
                b.report_latency_us);
    if (mm) {
        std::printf("  strategy         MarketMaker (a toy): quote size %" PRIu64 ", max position %" PRId64
                    ", requote cooldown %" PRIu64 " ms, joins the best bid and ask\n",
                    b.quote_size, b.max_position, b.requote_ms);
    } else {
        std::printf("  strategy         ImbalanceTaker (a toy): threshold %g, clip %" PRIu64
                    ", max position %" PRId64 ", cooldown %" PRIu64 " ms\n",
                    b.threshold, b.clip, b.max_position, b.cooldown_ms);
    }
    std::printf("  risk             max position %" PRId64 ", max order %" PRIu64 ", max open orders %zu\n",
                limits.max_position, limits.max_order_quantity.value(), limits.max_open_orders);
    std::printf("  fees             maker %s, taker %s per share\n",
                usd(fees.maker_fee_per_share, price_scale).c_str(), usd(fees.taker_fee_per_share, price_scale).c_str());

    ExecutionSimulator execution{midnight};
    Portfolio portfolio{instrument, limits, fees};
    const ReplayOptions options{.check_invariants_every = args.check_invariants_every, .session_midnight = midnight};

    auto run = [&]<Strategy S>(S& strategy) {
        EventLoop<S> loop{source, book, execution, portfolio, strategy, latency, options};

        const auto start = std::chrono::steady_clock::now();
        const LoopReport report = loop.run();
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        const ReplayReport& replay = report.replay;

        std::printf("\nReplay\n");
        std::printf("  events           %" PRIu64, replay.events);
        if (replay.events != 0) {
            std::printf(", %s to %s local", clock_time(replay.first_timestamp, midnight).c_str(),
                        clock_time(replay.last_timestamp, midnight).c_str());
        }
        std::printf("\n");
        print_applied(replay, "  ");
        std::printf("  sim events       %" PRIu64 " handled (%" PRIu64 " drained after end of stream), %" PRIu64
                    " discarded\n",
                    report.sim_events, report.drained_sim_events, report.discarded_sim_events);
        print_failure(replay, args);

        print_strategy(strategy.stats());
        print_execution(execution.statistics());
        print_attribution(execution.statistics());
        print_anomaly_samples(execution, midnight, price_scale);

        std::printf("\nPortfolio\n");
        std::printf("  position         %" PRId64 " shares\n", portfolio.position());
        std::printf("  cash             %s\n", usd(portfolio.cash(), money_scale).c_str());
        std::printf("  realised PnL     %s\n", usd(portfolio.realised_pnl(), money_scale).c_str());
        std::printf("  unrealised PnL   %s\n", usd(portfolio.unrealised_pnl(), money_scale).c_str());
        std::printf("  fees             %s\n", usd(portfolio.fees(), money_scale).c_str());
        std::printf("  total PnL        %s (realised + unrealised - fees)\n",
                    usd(portfolio.pnl(), money_scale).c_str());
        std::printf("  max drawdown     %s\n", usd(portfolio.max_drawdown(), money_scale).c_str());
        std::printf("  max |position|   %" PRId64 " shares\n", portfolio.max_abs_position());
        std::printf("  open orders      %zu at the end\n", portfolio.open_orders());

        const bool invariants = print_final_book(book, replay, args.depth, price_scale, midnight);

        const double seconds = elapsed.count();
        std::printf("Elapsed           %.1f ms, %.0f events/s\n", seconds * 1e3,
                    static_cast<double>(replay.events) / seconds);

        return replay.succeeded() && invariants ? 0 : 1;
    };

    if (mm) {
        MarketMaker strategy{mm_params};
        return run(strategy);
    }
    ImbalanceTaker strategy{imbalance_params};
    return run(strategy);
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    ReplayArgs parsed;
    std::string error;

    if (!parse_replay_args(args, parsed, error)) {
        std::fprintf(stderr, "error: %s\n\n%.*s", error.c_str(), static_cast<int>(usage.size()), usage.data());
        return 2;
    }
    if (parsed.command == CliCommand::Backtest) {
        return backtest(parsed);
    }
    return parsed.symbol.empty() ? replay_books(parsed) : replay(parsed);
}
