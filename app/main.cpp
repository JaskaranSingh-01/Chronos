#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "simulator/cli/replay_args.hpp"
#include "simulator/engine/event_loop.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/strategies/imbalance_taker.hpp"

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
    std::printf("  symbol           %.*s\n", static_cast<int>(args.symbol.size()), args.symbol.data());
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
    std::printf("  capacity         %zu orders\n", args.capacity);
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

// MarketReplay plus the ImbalanceTaker toy through EventLoop: IOC orders
// filled against the replayed book (never changing it), with latency, risk
// and fees. Money prints in the instrument's currency.
int backtest(const ReplayArgs& args)
{
    MappedFile file;
    if (!file.open(std::string{args.file}.c_str())) {
        std::fprintf(stderr, "error: %s\n", file.error());
        return 2;
    }

    const BacktestArgs& b = args.backtest;
    const ItchSourceConfig config{.ticker = args.symbol, .session_date = args.date,
                                  .utc_offset = args.utc_offset,
                                  .allow_truncated_tail = args.allow_truncated_tail};
    const Timestamp midnight = session_midnight_ns(args.date, args.utc_offset);
    const LatencyConfig latency{.order_latency = static_cast<Timestamp>(b.order_latency_us) * 1'000,
                                .report_latency = static_cast<Timestamp>(b.report_latency_us) * 1'000};
    const ImbalanceTakerParams params{.threshold = b.threshold, .clip = b.clip, .max_position = b.max_position,
                                      .cooldown_ns = static_cast<Timestamp>(b.cooldown_ms) * 1'000'000};
    // One IOC in flight at a time; the strategy's own position limit, enforced again pre-trade.
    const RiskLimits limits{.max_position = b.max_position, .max_order_quantity = Quantity{b.clip},
                            .max_open_orders = 1};
    const FeeSchedule fees{.taker_fee_per_share = static_cast<std::int64_t>(b.taker_fee_ticks)};

    InstrumentRegistry registry{1};
    ItchSource source{file.bytes(), config, registry};
    MarketBook book{0, args.capacity};  // the registry's first id: ItchSource registers the only instrument
    const Instrument& instrument = source.instrument();
    const unsigned price_scale = instrument.price_scale;
    const unsigned money_scale = instrument.price_scale + instrument.quantity_scale;

    print_config("backtest", args, file);
    std::printf("  latency          order %" PRIu64 " us, report %" PRIu64 " us\n", b.order_latency_us,
                b.report_latency_us);
    std::printf("  strategy         ImbalanceTaker (a toy): threshold %g, clip %" PRIu64 ", max position %" PRId64
                ", cooldown %" PRIu64 " ms\n",
                b.threshold, b.clip, b.max_position, b.cooldown_ms);
    std::printf("  risk             max position %" PRId64 ", max order %" PRIu64 ", max open orders %zu\n",
                limits.max_position, limits.max_order_quantity.value(), limits.max_open_orders);
    std::printf("  taker fee        %s per share\n", usd(fees.taker_fee_per_share, price_scale).c_str());

    ExecutionSimulator execution;
    Portfolio portfolio{instrument, limits, fees};
    ImbalanceTaker strategy{params};
    EventLoop<ImbalanceTaker> loop{
        source, book, execution, portfolio, strategy, latency,
        ReplayOptions{.check_invariants_every = args.check_invariants_every, .session_midnight = midnight}};

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
    std::printf("  sim events       %" PRIu64 " handled, %" PRIu64 " discarded at end of stream\n",
                report.sim_events, report.discarded_sim_events);
    print_failure(replay, args);

    const ImbalanceTakerStats& stats = strategy.stats();
    std::printf("\nStrategy\n");
    std::printf("  orders sent      %" PRIu64 "\n", stats.sent);
    std::printf("  risk rejects     %" PRIu64 "\n", stats.risk_rejects);
    std::printf("  fills            %" PRIu64 "\n", stats.fills);
    std::printf("  expired          %" PRIu64 "\n", stats.expired);
    std::printf("  bought           %" PRIu64 " shares\n", stats.bought);
    std::printf("  sold             %" PRIu64 " shares\n", stats.sold);

    std::printf("\nPortfolio\n");
    std::printf("  position         %" PRId64 " shares\n", portfolio.position());
    std::printf("  cash             %s\n", usd(portfolio.cash(), money_scale).c_str());
    std::printf("  realised PnL     %s\n", usd(portfolio.realised_pnl(), money_scale).c_str());
    std::printf("  unrealised PnL   %s\n", usd(portfolio.unrealised_pnl(), money_scale).c_str());
    std::printf("  fees             %s\n", usd(portfolio.fees(), money_scale).c_str());
    std::printf("  total PnL        %s (realised + unrealised - fees)\n", usd(portfolio.pnl(), money_scale).c_str());
    std::printf("  max drawdown     %s\n", usd(portfolio.max_drawdown(), money_scale).c_str());
    std::printf("  max |position|   %" PRId64 " shares\n", portfolio.max_abs_position());

    const bool invariants = print_final_book(book, replay, args.depth, price_scale, midnight);

    const double seconds = elapsed.count();
    std::printf("Elapsed           %.1f ms, %.0f events/s\n", seconds * 1e3,
                static_cast<double>(replay.events) / seconds);

    return replay.succeeded() && invariants ? 0 : 1;
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
    return parsed.command == CliCommand::Backtest ? backtest(parsed) : replay(parsed);
}
