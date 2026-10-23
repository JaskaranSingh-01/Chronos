#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace simulator {

// Help text for every command; a test requires each flag to appear here as " --flag ".
inline constexpr std::string_view usage =
    "usage: simulator replay <itch-file> (--symbol AAPL | --symbols AAPL,MSFT | --all)\n"
    "                        --date 2019-12-30 [options]\n"
    "       simulator replay --store DIR --symbol AAPL --date 2019-12-30 [options]\n"
    "       simulator backtest (<itch-file> | --store DIR) --symbol AAPL --date 2019-12-30 [options]\n"
    "                          [backtest options]\n"
    "       simulator -h | --help | --version\n"
    "\n"
    "options, both commands:\n"
    "  --symbol AAPL           one symbol, 1-8 characters (backtest takes only --symbol)\n"
    "  --date YYYY-MM-DD       the session date (required)\n"
    "  --store DIR             read the --symbol's day from the per-symbol store chronos-serve builds\n"
    "                          (its --store, by default <data>/store) in place of <itch-file>\n"
    "  --utc-offset -05:00     offset from UTC (+HH:MM or -HH:MM); default US Eastern for --date\n"
    "  --allow-truncated-tail  a file cut mid-message ends there instead of failing\n"
    "  --depth N               book levels to print per side, at most 1000 (default 5)\n"
    "  --check-invariants N    check the book every N applied events (default 0 = only at the end)\n"
    "  --capacity N            order slots reserved for the book, at most 1073741824 (default 1048576);\n"
    "                          with --symbols or --all, shared by every book (default 4194304)\n"
    "  --json PATH             also write the results, metrics, build and input as JSON;\n"
    "                          backtest adds its config, fills, fees and PnL\n"
    "\n"
    "replay only:\n"
    "  --symbols A,B,C         several symbols in one pass, each into its own book\n"
    "  --all                   every symbol in the file\n"
    "  --latency               time every batch of 1024 events; print ns/event percentiles\n"
    "                          over the batches\n"
    "  --latency-batch N       events per timed batch (implies --latency)\n"
    "\n"
    "backtest runs a toy strategy against the replayed book, which our orders never change:\n"
    "  --strategy S            imbalance (default): IOCs on top-of-book imbalance;\n"
    "                          mm: post-only quotes at the best bid and ask\n"
    "  --order-latency-ns N    strategy to exchange, in nanoseconds (default 50000)\n"
    "  --order-latency-us N    the same in microseconds\n"
    "  --report-latency-ns N   exchange back to strategy and portfolio (default 50000)\n"
    "  --report-latency-us N   the same in microseconds\n"
    "risk limits (the largest order is the strategy's own --clip or --quote-size):\n"
    "  --max-position N        |position| limit in shares (default 500)\n"
    "  --max-open-orders N     live orders at once (default 1 for imbalance, 2 for mm)\n"
    "fees, per share in price ticks ($0.0001 for ITCH):\n"
    "  --taker-fee-ticks N     paid when taking liquidity (default 0)\n"
    "  --maker-fee-ticks N     paid when providing it; negative = rebate (default 0)\n"
    "imbalance only:\n"
    "  --threshold X           trade when one top-of-book size >= X times the other (>= 1, default 3)\n"
    "  --clip N                shares per order (default 100)\n"
    "  --cooldown-ms N         wait after each order (default 100)\n"
    "mm only:\n"
    "  --quote-size N          shares per quote (default 100)\n"
    "  --requote-ms N          a quote lives at least this long before it is moved (default 100)\n"
    "\n"
    "  -h, --help              print this help\n"
    "  --version               print the commit, build type and compiler\n"
    "\n"
    "exit status:\n"
    "  0  the run succeeded and every check passed (also --help and --version)\n"
    "  1  the run failed: a source error (a file cut mid-message without --allow-truncated-tail,\n"
    "     no stock directory message for --symbol, ...), an out-of-order timestamp, or a failed\n"
    "     book invariant (e.g. a book crossed in regular trading hours while it is trading);\n"
    "     rejected events alone are counted, not failures\n"
    "  2  bad usage, an input file that cannot be opened, a symbol the store does not hold on --date,\n"
    "     a store that cannot be read, or a --json file that cannot be written\n";

enum class CliCommand : std::uint8_t {
    Replay,
    Backtest,
    Help,
    Version
};

enum class StrategyKind : std::uint8_t {
    Imbalance,
    MarketMaker
};

struct BacktestArgs
{
    StrategyKind strategy{StrategyKind::Imbalance};
    std::uint64_t order_latency_ns{50'000};  // --order-latency-ns, or -us x 1'000
    std::uint64_t report_latency_ns{50'000};
    std::int64_t max_position{500};
    std::size_t max_open_orders{0};  // 0 = default_max_open_orders(strategy)
    std::uint64_t taker_fee_ticks{0};
    double threshold{3.0};           // imbalance
    std::uint64_t clip{100};
    std::uint64_t cooldown_ms{100};
    std::uint64_t quote_size{100};   // mm
    std::uint64_t requote_ms{100};
    std::int64_t maker_fee_ticks{0};
};

// One IOC in flight for imbalance; one quote per side for mm.
constexpr std::size_t default_max_open_orders(StrategyKind strategy) noexcept
{
    return strategy == StrategyKind::MarketMaker ? 2 : 1;
}

// Default --capacity of the pool shared by --symbols/--all: 2^22, over twice the
// 1,924,078 peak live orders of the 2019-12-30 full day.
inline constexpr std::size_t default_books_capacity = 4'194'304;

// Events per timed batch for --latency: a batch takes ~0.5 ms, far above the
// 41.67 ns tick of steady_clock on Apple silicon.
inline constexpr std::uint64_t default_latency_batch = 1'024;

struct ReplayArgs
{
    CliCommand command{CliCommand::Replay};
    std::string_view file;
    std::string_view store;                 // --store: the per-symbol store to read --symbol from, in place of file
    std::string_view symbol;                // 1-8 chars, an ITCH ticker
    std::vector<std::string_view> symbols;  // --symbols, each like symbol
    bool all_symbols{false};                // --all
    std::chrono::year_month_day date{};
    std::chrono::minutes utc_offset{0};  // --utc-offset, else us_eastern_utc_offset(date)
    bool allow_truncated_tail{false};
    std::size_t depth{5};
    std::uint64_t check_invariants_every{0};
    std::size_t capacity{1'048'576};
    std::uint64_t latency_batch{0};  // replay --latency: events per timed batch; 0 = off
    std::string_view json;           // --json: where to write the run as JSON
    BacktestArgs backtest{};
};

// Parses argv minus the program name: command, file, then flags in any order.
// -h, --help or --version anywhere wins. On false, `error` says what is wrong.
bool parse_replay_args(std::span<const std::string_view> args, ReplayArgs& out, std::string& error);

// "YYYY-MM-DD", a real calendar date.
bool parse_date(std::string_view text, std::chrono::year_month_day& date) noexcept;

// "+HH:MM" or "-HH:MM", HH < 24, MM < 60.
bool parse_utc_offset(std::string_view text, std::chrono::minutes& offset) noexcept;

} // namespace simulator
