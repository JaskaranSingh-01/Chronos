#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <sys/sysctl.h>
#endif

#include "alloc_counter.hpp"
#include "build_info.hpp"  // generated: build_commit, build_dirty, build_type
#include "simulator/cli/replay_args.hpp"
#include "simulator/engine/event_loop.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_books.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/metrics/latency_histogram.hpp"
#include "simulator/store/store.hpp"
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
    const std::uint64_t magnitude =
        ticks < 0 ? 0 - static_cast<std::uint64_t>(ticks) : static_cast<std::uint64_t>(ticks);
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
    std::snprintf(text, sizeof text, "%02lld:%02lld:%02lld.%09lld", static_cast<long long>(ns / (3600 * second)),
                  static_cast<long long>(ns / (60 * second) % 60), static_cast<long long>(ns / second % 60),
                  static_cast<long long>(ns % second));
    return text;
}

void print_levels(const char* side, const std::vector<LevelInfo>& levels, unsigned scale)
{
    for (const LevelInfo& level : levels) {
        std::printf("  %s %14s %12" PRIu64 " %8u\n", side, dollars(level.price.ticks, scale).c_str(), level.quantity,
                    level.order_count);
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
    const std::string_view state = to_string(book.trading_state());
    std::printf("  spread %s, orders %zu, peak orders %zu, crossed %s, locked %s, trading state %.*s\n", spread.c_str(),
                book.order_count(), book.peak_order_count(), book.is_crossed() ? "yes" : "no",
                book.is_locked() ? "yes" : "no", static_cast<int>(state.size()), state.data());
}

// What both commands replay: the day file, mapped, or with --store the symbol's segment of the day, read into
// `segment`. False, said on stderr, if it cannot be read.
bool open_input(const ReplayArgs& args, MappedFile& file, std::vector<std::byte>& segment,
                std::span<const std::byte>& input)
{
    if (args.store.empty()) {
        if (!file.open(std::string{args.file}.c_str())) {
            std::fprintf(stderr, "error: %s\n", file.error());
            return false;
        }
        input = file.bytes();
        return true;
    }
    const std::filesystem::path store{args.store};
    StoreIndex index;
    std::string error;
    if (!index.load(store, error)) {
        std::fprintf(stderr, "error: the store index at %s is invalid; chronos-serve rebuilds it\n", store.c_str());
        return false;
    }
    const auto day = index.days().find(args.date);
    if (day == index.days().end() || !day->second.segments.contains(args.symbol)) {
        std::fprintf(stderr, "error: no %.*s on %04d-%02u-%02u in %s\n", static_cast<int>(args.symbol.size()),
                     args.symbol.data(), static_cast<int>(args.date.year()), static_cast<unsigned>(args.date.month()),
                     static_cast<unsigned>(args.date.day()), store.c_str());
        return false;
    }
    if (!read_segment(store, args.symbol, args.date, day->second.segments.find(args.symbol)->second, segment, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }
    input = segment;
    return true;
}

// The run header both commands print.
void print_config(const char* command, const ReplayArgs& args, std::span<const std::byte> input)
{
    const auto offset = args.utc_offset.count();

    std::printf("Chronos %s\n", command);
    if (args.store.empty()) {
        std::printf("  file             %.*s (%zu bytes)\n", static_cast<int>(args.file.size()), args.file.data(),
                    input.size());
    } else {
        std::printf("  store            %.*s (the symbol's segment, %zu bytes)\n", static_cast<int>(args.store.size()),
                    args.store.data(), input.size());
    }
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
    std::printf("  session          %04d-%02u-%02u, UTC%c%02lld:%02lld\n", static_cast<int>(args.date.year()),
                static_cast<unsigned>(args.date.month()), static_cast<unsigned>(args.date.day()),
                offset < 0 ? '-' : '+', static_cast<long long>(std::abs(offset) / 60),
                static_cast<long long>(std::abs(offset) % 60));
    std::printf("  truncated tail   %s\n", args.allow_truncated_tail ? "allowed" : "fatal");
    if (args.check_invariants_every == 0) {
        std::printf("  invariant checks at the end only\n");
    } else {
        std::printf("  invariant checks every %" PRIu64 " applied events, and at the end\n",
                    args.check_invariants_every);
    }
    std::printf("  capacity         %zu orders%s\n", args.capacity,
                args.symbol.empty() ? ", shared by every book" : "");
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

void print_failure(const ReplayReport& report, const ReplayArgs& args, std::uint8_t failed_message_type,
                   const BookStorage& storage)
{
    if (report.succeeded()) {
        return;
    }
    std::printf("\nFAILED: %.*s", static_cast<int>(report.message.size()), report.message.data());
    if (failed_message_type != 0) {
        std::printf(" in message type '%c'", failed_message_type);
    }
    std::printf(" at position %" PRIu64 " (byte offset)\n", report.failed_position);
    if (report.source_error == SourceError::TruncatedMessage) {
        std::printf("  hint: the file ends inside a message (a slice cut at a byte count?);"
                    " pass --allow-truncated-tail to stop there instead\n");
    } else if (report.source_error == SourceError::SymbolNotFound) {
        std::printf("  hint: no stock directory (R) message for %.*s in this file\n",
                    static_cast<int>(args.symbol.size()), args.symbol.data());
    } else if (report.failure == ReplayFailure::CapacityExhausted) {
        const std::string_view full = to_string(report.exhausted);
        std::printf("  hint: %.*s (%zu) is full; raise --capacity\n", static_cast<int>(full.size()), full.data(),
                    storage.capacity(report.exhausted));
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
    std::printf("\nInvariants at end: %s%.*s (%" PRIu64 " periodic checks passed)\n", invariants ? "ok" : "FAILED ",
                invariants ? 0 : static_cast<int>(invariant_name.size()), invariant_name.data(),
                report.invariant_checks);
    std::printf("Digest            %016" PRIx64 "\n", book.digest());
    return invariants;
}

// Process counters at one instant.
struct Usage
{
    std::chrono::steady_clock::time_point wall;
    double user_s{0};
    double sys_s{0};
    std::uint64_t allocations{0};
    std::uint64_t major_faults{0};
};

Usage usage_now() noexcept
{
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    auto seconds = [](const timeval& t) {
        return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) * 1e-6;
    };
    return {std::chrono::steady_clock::now(), seconds(usage.ru_utime), seconds(usage.ru_stime), allocation_count(),
            static_cast<std::uint64_t>(usage.ru_majflt)};
}

// What one run() cost.
struct RunMetrics
{
    double wall_s{0};
    double user_s{0};
    double sys_s{0};
    std::uint64_t allocations{0};      // operator new calls during run()
    std::uint64_t major_faults{0};     // page faults that waited for I/O
    std::uint64_t peak_rss{0};         // bytes, whole process, including resident pages of the mapped file
    std::uint64_t peak_footprint{0};   // bytes, macOS only; excludes clean file pages such as the mapped file
    const LatencyHistogram* latency{nullptr};  // --latency only
    std::uint64_t latency_batch{0};
};

// Runs run() and records its wall and CPU time, allocations, faults and the process peaks.
template <class Run>
auto measured(Run&& run, RunMetrics& metrics)
{
    const Usage before = usage_now();
    auto result = run();
    const Usage after = usage_now();

    metrics.wall_s = std::chrono::duration<double>(after.wall - before.wall).count();
    metrics.user_s = after.user_s - before.user_s;
    metrics.sys_s = after.sys_s - before.sys_s;
    metrics.allocations = after.allocations - before.allocations;
    metrics.major_faults = after.major_faults - before.major_faults;

    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    metrics.peak_rss = static_cast<std::uint64_t>(usage.ru_maxrss);  // bytes on macOS
    rusage_info_v4 info{};
    if (::proc_pid_rusage(::getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) == 0) {
        metrics.peak_footprint = info.ri_lifetime_max_phys_footprint;
    }
#else
    metrics.peak_rss = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;  // KiB on Linux
#endif
    return result;
}

// Smallest observed steady_clock step: ~42 ns on Apple silicon, so --latency times batches, not events.
double timer_tick_ns() noexcept
{
    using clock = std::chrono::steady_clock;
    clock::duration best = clock::duration::max();
    for (int i = 0; i < 100; ++i) {
        const clock::time_point start = clock::now();
        clock::time_point now = clock::now();
        while (now == start) {
            now = clock::now();
        }
        best = std::min(best, now - start);
    }
    return std::chrono::duration<double, std::nano>(best).count();
}

// a / b, or 0 for an empty run: keeps the JSON free of inf and nan.
double ratio(double a, double b) noexcept
{
    return b != 0 ? a / b : 0;
}

// One replay's outcome, for the summary lines and --json.
struct RunRecord
{
    std::uint64_t events{0};
    std::uint64_t messages{0};
    std::uint64_t rejected{0};
    std::uint64_t digest{0};
    std::size_t books{0};
    std::size_t books_not_empty{0};
    std::size_t peak_live_orders{0};
    bool succeeded{false};  // the source and every check, as the exit code says
    RunMetrics metrics;
};

void print_metrics(const RunRecord& run, std::uint64_t file_bytes)
{
    const RunMetrics& m = run.metrics;
    const auto events = static_cast<double>(run.events);
    std::printf("Elapsed           %.2f s wall, %.2f s CPU (user %.2f, sys %.2f); %.0f events/s, %.1f ns/event, "
                "%.0f messages/s, %.1f MB/s\n",
                m.wall_s, m.user_s + m.sys_s, m.user_s, m.sys_s, ratio(events, m.wall_s), ratio(m.wall_s * 1e9, events),
                ratio(static_cast<double>(run.messages), m.wall_s),
                ratio(static_cast<double>(file_bytes) / 1e6, m.wall_s));
    std::printf("Memory            %" PRIu64 " allocations (%.4f per event), peak RSS %.1f MB, peak footprint "
                "%.1f MB, %" PRIu64 " major faults\n",
                m.allocations, ratio(static_cast<double>(m.allocations), events), static_cast<double>(m.peak_rss) / 1e6,
                static_cast<double>(m.peak_footprint) / 1e6, m.major_faults);
    if (m.latency != nullptr) {
        const LatencyHistogram& h = *m.latency;
        std::printf("Latency           ns/event over %" PRIu64 " batches of %" PRIu64 " events (timer tick %.1f ns): "
                    "p50 %" PRIu64 ", p90 %" PRIu64 ", p99 %" PRIu64 ", p99.9 %" PRIu64 ", max %" PRIu64 "\n",
                    h.count(), m.latency_batch, timer_tick_ns(), h.percentile(0.5), h.percentile(0.9),
                    h.percentile(0.99), h.percentile(0.999), h.max());
    }
}

// JSON string contents: quotes, backslashes and control characters escaped.
std::string json_escape(std::string_view text)
{
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
            out += escaped;
        } else {
            out += c;
        }
    }
    return out;
}

// FNV-1a 64 of the first and last 4 KiB: cheap, and tells same-size files apart.
std::uint64_t fingerprint(std::span<const std::byte> bytes) noexcept
{
    const std::size_t edge = std::min<std::size_t>(4'096, bytes.size());
    std::uint64_t hash = fnv1a_offset_basis;
    for (const std::span<const std::byte> part : {bytes.first(edge), bytes.last(edge)}) {
        for (const std::byte byte : part) {
            hash ^= std::to_integer<std::uint64_t>(byte);
            hash *= 1099511628211ULL;  // FNV-1a 64 prime
        }
    }
    return hash;
}

std::string cpu_model()
{
#ifdef __APPLE__
    char name[256] = {};
    std::size_t size = sizeof name - 1;
    if (::sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0) {
        return name;
    }
#endif
    return "unknown";
}

// "--all", the --symbols list or the --symbol.
std::string symbols_mode(const ReplayArgs& args)
{
    if (args.all_symbols) {
        return "all";
    }
    if (args.symbols.empty()) {
        return std::string{args.symbol};
    }
    std::string list;
    for (const std::string_view symbol : args.symbols) {
        list += (list.empty() ? "" : ",") + std::string{symbol};
    }
    return list;
}

// Writes --json: build, machine, input, then the run, with `extra` (a complete
// member) just before "latency". False if the file cannot be written.
bool write_json(const ReplayArgs& args, std::span<const std::byte> input, const RunRecord& run,
                std::string_view extra = {})
{
    const std::string path{args.json};
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "error: cannot write %s: %s\n", path.c_str(), std::strerror(errno));
        return false;
    }

    char date[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", &utc);
    utsname os{};
    ::uname(&os);
    const RunMetrics& m = run.metrics;
    const auto events = static_cast<double>(run.events);

    std::fprintf(out, "{\n  \"schema\": 1,\n  \"date\": \"%s\",\n", date);
    std::fprintf(out, "  \"build\": {\"commit\": \"%s\", \"dirty\": %s, \"type\": \"%s\", \"compiler\": \"%s\"},\n",
                 build_commit, build_dirty ? "true" : "false", build_type, json_escape(__VERSION__).c_str());
    std::fprintf(out, "  \"machine\": {\"cpu\": \"%s\", \"cores\": %ld, \"memory_bytes\": %lld, \"os\": \"%s %s\"},\n",
                 json_escape(cpu_model()).c_str(), ::sysconf(_SC_NPROCESSORS_ONLN),
                 static_cast<long long>(::sysconf(_SC_PHYS_PAGES)) * ::sysconf(_SC_PAGESIZE), os.sysname, os.release);
    std::fprintf(out, "  \"input\": {\"path\": \"%s\", \"bytes\": %zu, \"fingerprint\": \"%016" PRIx64 "\"},\n",
                 json_escape(args.store.empty() ? args.file : args.store).c_str(), input.size(), fingerprint(input));
    std::fprintf(out, "  \"run\": {\"symbols\": \"%s\", \"capacity\": %zu, \"check_invariants_every\": %" PRIu64 "},\n",
                 json_escape(symbols_mode(args)).c_str(), args.capacity, args.check_invariants_every);
    std::fprintf(out,
                 "  \"result\": {\"events\": %" PRIu64 ", \"messages\": %" PRIu64 ", \"rejected\": %" PRIu64
                 ", \"digest\": \"%016" PRIx64 "\", \"books\": %zu, \"books_not_empty\": %zu, "
                 "\"peak_live_orders\": %zu, \"succeeded\": %s},\n",
                 run.events, run.messages, run.rejected, run.digest, run.books, run.books_not_empty,
                 run.peak_live_orders, run.succeeded ? "true" : "false");
    std::fprintf(out,
                 "  \"time\": {\"wall_s\": %.3f, \"user_s\": %.3f, \"sys_s\": %.3f, \"cpu_s\": %.3f, "
                 "\"events_per_s\": %.0f, \"ns_per_event\": %.2f},\n",
                 m.wall_s, m.user_s, m.sys_s, m.user_s + m.sys_s, ratio(events, m.wall_s),
                 ratio(m.wall_s * 1e9, events));
    std::fprintf(
        out,
        "  \"memory\": {\"allocations\": %" PRIu64 ", \"allocations_per_event\": %.4f, "
        "\"peak_rss_bytes\": %" PRIu64 ", \"peak_footprint_bytes\": %" PRIu64 ", \"major_faults\": %" PRIu64 "},\n",
        m.allocations, ratio(static_cast<double>(m.allocations), events), m.peak_rss, m.peak_footprint, m.major_faults);
    std::fprintf(out, "%.*s", static_cast<int>(extra.size()), extra.data());
    if (m.latency == nullptr) {
        std::fprintf(out, "  \"latency\": null\n}\n");
    } else {
        const LatencyHistogram& h = *m.latency;
        std::fprintf(out,
                     "  \"latency\": {\"batch_events\": %" PRIu64 ", \"batches\": %" PRIu64
                     ", \"timer_tick_ns\": %.2f, "
                     "\"ns_per_event\": {\"p50\": %" PRIu64 ", \"p90\": %" PRIu64 ", \"p99\": %" PRIu64
                     ", \"p99_9\": %" PRIu64 ", \"max\": %" PRIu64 "}}\n}\n",
                     m.latency_batch, h.count(), timer_tick_ns(), h.percentile(0.5), h.percentile(0.9),
                     h.percentile(0.99), h.percentile(0.999), h.max());
    }
    return std::fclose(out) == 0;
}

// The result fields of a one-book run, replay or backtest alike.
void fill_record(RunRecord& run, const ReplayReport& report, const MarketBook& book, std::uint64_t messages,
                 bool invariants) noexcept
{
    run.events = report.events;
    run.messages = messages;
    run.rejected = report.rejected();
    run.digest = book.digest();
    run.books = 1;
    run.books_not_empty = book.order_count() != 0;
    run.peak_live_orders = book.peak_order_count();
    run.succeeded = report.succeeded() && invariants;
}

// backtest --json: the numbers backtest prints, as exact integers; money in
// units of 10^-money_scale, fees per share in price ticks (10^-price_scale).
std::string backtest_json(const BacktestArgs& b, const RiskLimits& limits, const Instrument& instrument,
                          const LoopReport& loop, const ExecutionStatistics& e, const Portfolio& p)
{
    char text[4'096];  // about 1,300 bytes at most: 30 numbers of at most 20 digits
    std::snprintf(
        text, sizeof text,
        "  \"backtest\": {\"strategy\": \"%s\", \"order_latency_ns\": %" PRIu64 ", \"report_latency_ns\": %" PRIu64
        ", \"max_position\": %" PRId64 ", \"max_order_quantity\": %" PRIu64 ", \"max_open_orders\": %zu"
        ", \"maker_fee_ticks\": %" PRId64 ", \"taker_fee_ticks\": %" PRIu64
        ", \"price_scale\": %u, \"money_scale\": %u,\n"
        "    \"sim_events\": %" PRIu64 ", \"drained_sim_events\": %" PRIu64 ", \"discarded_sim_events\": %" PRIu64 ",\n"
        "    \"maker_fills\": %" PRIu64 ", \"maker_quantity\": %" PRIu64 ", \"taker_fills\": %" PRIu64
        ", \"taker_quantity\": %" PRIu64 ", \"accepted_quantity\": %" PRIu64 ", \"cancelled\": %" PRIu64
        ", \"cancel_rejects\": %" PRIu64 ", \"would_cross_rejects\": %" PRIu64 ", \"not_trading_rejects\": %" PRIu64
        ", \"fifo_anomalies\": %" PRIu64 ",\n"
        "    \"position\": %" PRId64 ", \"max_abs_position\": %" PRId64 ", \"open_orders\": %zu, \"cash\": %" PRId64
        ", \"realised_pnl\": %" PRId64 ", \"unrealised_pnl\": %" PRId64 ", \"fees\": %" PRId64 ", \"pnl\": %" PRId64
        ", \"max_drawdown\": %" PRId64 "},\n",
        b.strategy == StrategyKind::MarketMaker ? "mm" : "imbalance", b.order_latency_ns, b.report_latency_ns,
        limits.max_position, limits.max_order_quantity.value(), limits.max_open_orders, b.maker_fee_ticks,
        b.taker_fee_ticks, unsigned{instrument.price_scale},
        unsigned{instrument.price_scale} + instrument.quantity_scale, loop.sim_events, loop.drained_sim_events,
        loop.discarded_sim_events, e.maker_fills, e.maker_quantity, e.taker_fills, e.taker_quantity,
        e.accepted_quantity, e.cancelled, e.cancel_rejects, e.would_cross_rejects, e.not_trading_rejects,
        e.fifo_anomalies, p.position(), p.max_abs_position(), p.open_orders(), p.cash(), p.realised_pnl(),
        p.unrealised_pnl(), p.fees(), p.pnl(), p.max_drawdown());
    return text;
}

// The options both replays share, with --latency wired to `latency`.
ReplayOptions replay_options(const ReplayArgs& args, Timestamp midnight, LatencyHistogram& latency) noexcept
{
    ReplayOptions options{.check_invariants_every = args.check_invariants_every, .session_midnight = midnight};
    if (args.latency_batch != 0) {
        options.latency = &latency;
        options.latency_batch = args.latency_batch;
    }
    return options;
}

int replay(const ReplayArgs& args)
{
    MappedFile file;
    std::vector<std::byte> segment;
    std::span<const std::byte> input;
    if (!open_input(args, file, segment, input)) {
        return 2;
    }

    const ItchSourceConfig config{.ticker = args.symbol,
                                  .session_date = args.date,
                                  .utc_offset = args.utc_offset,
                                  .allow_truncated_tail = args.allow_truncated_tail};
    const Timestamp midnight = session_midnight_ns(args.date, args.utc_offset);

    print_config("replay", args, input);

    InstrumentRegistry registry{1};
    ItchSource source{input, config, registry};
    MarketBook book{0, args.capacity};  // the registry's first id: ItchSource registers the only instrument

    LatencyHistogram latency;
    const ReplayOptions options = replay_options(args, midnight, latency);
    RunRecord run;
    const ReplayReport report = measured([&] { return MarketReplay::run(source, book, options); }, run.metrics);

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
    print_failure(report, args, source.failed_message_type(), book.storage());

    const bool invariants = print_final_book(book, report, args.depth, source.instrument().price_scale, midnight);

    fill_record(run, report, book, stats.messages, invariants);
    run.metrics.latency = options.latency;
    run.metrics.latency_batch = options.latency_batch;
    print_metrics(run, input.size());

    if (!args.json.empty() && !write_json(args, input, run)) {
        return 2;
    }
    return run.succeeded ? 0 : 1;
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

    const ItchSourceConfig config{.tickers = args.symbols,
                                  .all_tickers = args.all_symbols,
                                  .session_date = args.date,
                                  .utc_offset = args.utc_offset,
                                  .allow_truncated_tail = args.allow_truncated_tail};
    const Timestamp midnight = session_midnight_ns(args.date, args.utc_offset);

    print_config("replay", args, file.bytes());

    // ITCH locates are 16-bit, so a day has at most 65,536 instruments.
    InstrumentRegistry registry{args.all_symbols ? std::size_t{1} << 16 : args.symbols.size()};
    ItchSource source{file.bytes(), config, registry};
    BookStorage storage{args.capacity};
    MarketBooks books{registry, storage};

    LatencyHistogram latency;
    const ReplayOptions options = replay_options(args, midnight, latency);
    RunRecord run;
    const BooksReplayReport report = measured([&] { return MarketReplay::run(source, books, options); }, run.metrics);
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
    print_failure(total, args, source.failed_message_type(), storage);
    if (total.failure == ReplayFailure::InvariantViolation || total.failure == ReplayFailure::CapacityExhausted) {
        const std::string_view failed = name(*registry.find(report.failed_symbol));
        std::printf("  in symbol %.*s\n", static_cast<int>(failed.size()), failed.data());
    }

    std::size_t non_empty = 0;
    for (const SymbolReport& symbol : report.symbols) {
        non_empty += symbol.orders_left != 0;
    }
    std::printf("\nBooks %zu: %zu empty at the end, %zu not empty\n", books.size(), books.size() - non_empty,
                non_empty);

    // --all: the 10 busiest, then up to 40 others with rejections or orders left, in SymbolId order.
    constexpr std::size_t max_others = 40;
    std::vector<SymbolId> rows(books.size());
    std::iota(rows.begin(), rows.end(), SymbolId{0});
    std::size_t hidden = 0;
    if (args.all_symbols) {
        std::stable_sort(rows.begin(), rows.end(),
                         [&](SymbolId a, SymbolId b) { return report.symbols[a].events > report.symbols[b].events; });
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
                report.peak_live_orders, storage.orders.in_use(), storage.orders.capacity());

    // Structure first, so a crossed book cannot hide another problem. A book that is
    // not Trading may cross (NASDAQ queues without matching), so it is listed, not failed.
    InvariantError invariant_error{InvariantError::None};
    SymbolId invariant_symbol{0};
    const bool structure = books.check_invariants(invariant_error, invariant_symbol);
    std::vector<SymbolId> crossed;
    std::vector<SymbolId> not_trading;  // crossed in RTH while not Trading: allowed
    if (in_rth(total.last_timestamp, midnight)) {
        for (SymbolId id = 0; id < books.size(); ++id) {
            if (books[id].is_crossed()) {
                (books[id].trading_state() == TradingState::Trading ? crossed : not_trading).push_back(id);
            }
        }
    }

    if (!structure) {
        const std::string_view error_name = to_string(invariant_error);
        const std::string_view failed =
            invariant_symbol < books.size() ? name(*registry.find(invariant_symbol)) : "(the shared storage)";
        std::printf("Invariants at end: FAILED %.*s in %.*s\n", static_cast<int>(error_name.size()), error_name.data(),
                    static_cast<int>(failed.size()), failed.data());
    } else if (!crossed.empty()) {
        std::printf("Invariants at end: FAILED CrossedInRth in %zu books:", crossed.size());
        for (std::size_t i = 0; i < crossed.size() && i < 20; ++i) {
            const std::string_view symbol = name(*registry.find(crossed[i]));
            std::printf(" %.*s", static_cast<int>(symbol.size()), symbol.data());
        }
        std::printf("%s; every other invariant ok in all %zu books\n", crossed.size() > 20 ? " ..." : "", books.size());
    } else {
        std::printf("Invariants at end: ok, all %zu books\n", books.size());
    }
    std::printf("                   %" PRIu64 " periodic checks passed\n", total.invariant_checks);
    if (!not_trading.empty()) {
        std::printf("Not trading       %zu books crossed in RTH, allowed:", not_trading.size());
        for (std::size_t i = 0; i < not_trading.size() && i < 20; ++i) {
            const std::string_view symbol = name(*registry.find(not_trading[i]));
            const std::string_view state = to_string(books[not_trading[i]].trading_state());
            std::printf(" %.*s (%.*s)", static_cast<int>(symbol.size()), symbol.data(), static_cast<int>(state.size()),
                        state.data());
        }
        std::printf("%s\n", not_trading.size() > 20 ? " ..." : "");
    }
    const bool invariants = structure && crossed.empty();
    std::printf("Digest (combined) %016" PRIx64 "\n", books.digest());

    run.events = total.events;
    run.messages = stats.messages;
    run.rejected = total.rejected();
    run.digest = books.digest();
    run.books = books.size();
    run.books_not_empty = non_empty;
    run.peak_live_orders = report.peak_live_orders;
    run.succeeded = total.succeeded() && invariants;
    run.metrics.latency = options.latency;
    run.metrics.latency_batch = options.latency_batch;
    print_metrics(run, file.bytes().size());

    if (!args.json.empty() && !write_json(args, file.bytes(), run)) {
        return 2;
    }
    return run.succeeded ? 0 : 1;
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
    std::printf("  rejects          %" PRIu64 " (would cross or not trading)\n", stats.rejects);
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
    std::printf("  not trading      %" PRIu64 " rejected\n", stats.not_trading_rejects);
    std::printf("  FIFO anomalies   %" PRIu64 "\n", stats.fifo_anomalies);
}

// Maker fills / FIFO anomalies by rule, historical message and session phase.
void print_attribution(const ExecutionStatistics& stats)
{
    static constexpr const char* causes[fill_cause_count] = {"(a) E", "(a) C",   "(d) E",
                                                             "(d) C", "(c) Add", "(c) Replace"};
    std::printf("\nFill attribution (maker fills/FIFO anomalies; crosses = first second of 09:30, 16:00)\n");
    std::printf("  %-12s %13s %13s %13s %13s %13s\n", "rule, msg", "pre-open", "open cross", "regular", "close cross",
                "post-close");
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

// MarketReplay plus a toy strategy through EventLoop: our orders fill against the
// replayed book without changing it, with latency, risk and fees.
int backtest(const ReplayArgs& args)
{
    MappedFile file;
    std::vector<std::byte> segment;
    std::span<const std::byte> input;
    if (!open_input(args, file, segment, input)) {
        return 2;
    }

    const BacktestArgs& b = args.backtest;
    const bool mm = b.strategy == StrategyKind::MarketMaker;
    const ItchSourceConfig config{.ticker = args.symbol,
                                  .session_date = args.date,
                                  .utc_offset = args.utc_offset,
                                  .allow_truncated_tail = args.allow_truncated_tail};
    const Timestamp midnight = session_midnight_ns(args.date, args.utc_offset);
    const LatencyConfig latency{.order_latency = static_cast<Timestamp>(b.order_latency_ns),
                                .report_latency = static_cast<Timestamp>(b.report_latency_ns)};
    const ImbalanceTakerParams imbalance_params{.threshold = b.threshold,
                                                .clip = b.clip,
                                                .max_position = b.max_position,
                                                .cooldown_ns = static_cast<Timestamp>(b.cooldown_ms) * 1'000'000};
    const MarketMakerParams mm_params{.quote_size = b.quote_size,
                                      .max_position = b.max_position,
                                      .requote_cooldown_ns = static_cast<Timestamp>(b.requote_ms) * 1'000'000};
    // The strategy's own position limit, enforced again pre-trade.
    const RiskLimits limits{.max_position = b.max_position,
                            .max_order_quantity = Quantity{mm ? b.quote_size : b.clip},
                            .max_open_orders =
                                b.max_open_orders != 0 ? b.max_open_orders : default_max_open_orders(b.strategy)};
    const FeeSchedule fees{.maker_fee_per_share = b.maker_fee_ticks,
                           .taker_fee_per_share = static_cast<std::int64_t>(b.taker_fee_ticks)};

    InstrumentRegistry registry{1};
    ItchSource source{input, config, registry};
    MarketBook book{0, args.capacity};  // the registry's first id: ItchSource registers the only instrument
    const Instrument& instrument = source.instrument();
    const unsigned price_scale = instrument.price_scale;
    const unsigned money_scale = instrument.price_scale + instrument.quantity_scale;

    print_config("backtest", args, input);
    std::printf("  latency          order %" PRIu64 " ns, report %" PRIu64 " ns\n", b.order_latency_ns,
                b.report_latency_ns);
    if (mm) {
        std::printf("  strategy         MarketMaker (a toy): quote size %" PRIu64 ", max position %" PRId64
                    ", requote cooldown %" PRIu64 " ms, joins the best bid and ask\n",
                    b.quote_size, b.max_position, b.requote_ms);
    } else {
        std::printf("  strategy         ImbalanceTaker (a toy): threshold %g, clip %" PRIu64 ", max position %" PRId64
                    ", cooldown %" PRIu64 " ms\n",
                    b.threshold, b.clip, b.max_position, b.cooldown_ms);
    }
    std::printf("  risk             max position %" PRId64 ", max order %" PRIu64 ", max open orders %zu\n",
                limits.max_position, limits.max_order_quantity.value(), limits.max_open_orders);
    std::printf("  fees             maker %s, taker %s per share\n", usd(fees.maker_fee_per_share, price_scale).c_str(),
                usd(fees.taker_fee_per_share, price_scale).c_str());

    ExecutionSimulator execution{midnight};
    Portfolio portfolio{instrument, limits, fees};
    const ReplayOptions options{.check_invariants_every = args.check_invariants_every, .session_midnight = midnight};

    auto run = [&]<Strategy S>(S& strategy) {
        EventLoop<S> loop{source, book, execution, portfolio, strategy, latency, options};

        RunRecord record;
        const LoopReport report = measured([&] { return loop.run(); }, record.metrics);
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
        print_failure(replay, args, source.failed_message_type(), book.storage());

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
        fill_record(record, replay, book, source.statistics().messages, invariants);
        print_metrics(record, input.size());

        if (!args.json.empty() &&
            !write_json(args, input, record,
                        backtest_json(b, limits, instrument, report, execution.statistics(), portfolio))) {
            return 2;
        }
        return record.succeeded ? 0 : 1;
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

    // Exit status, as --help documents it: 0 ok, 1 the run failed, 2 bad usage or file.
    if (!parse_replay_args(args, parsed, error)) {
        std::fprintf(stderr, "error: %s\nrun 'simulator --help' for the options and exit codes\n", error.c_str());
        return 2;
    }
    switch (parsed.command) {
    case CliCommand::Help:
        std::fwrite(usage.data(), 1, usage.size(), stdout);
        return 0;
    case CliCommand::Version:
        std::printf("Chronos simulator, commit %s%s, %s build, %s\n", build_commit, build_dirty ? " (dirty)" : "",
                    build_type, __VERSION__);
        return 0;
    case CliCommand::Backtest:
        return backtest(parsed);
    case CliCommand::Replay:
        break;
    }
    return parsed.symbol.empty() ? replay_books(parsed) : replay(parsed);
}
