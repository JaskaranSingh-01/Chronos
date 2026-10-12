#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace simulator {

inline constexpr std::string_view usage =
    "usage: simulator replay <itch-file> (--symbol AAPL | --symbols AAPL,MSFT | --all) --date 2019-12-30\n"
    "                        [--utc-offset -05:00] [--allow-truncated-tail] [--depth 5]\n"
    "                        [--check-invariants N] [--capacity N]\n"
    "       simulator backtest <itch-file> --symbol AAPL --date 2019-12-30 [replay options]\n"
    "                        [--strategy imbalance|mm] [--order-latency-us 50] [--report-latency-us 50]\n"
    "                        [--max-position 500] [--max-open-orders N] [--taker-fee-ticks 0]\n"
    "                        [--threshold 3] [--clip 100] [--cooldown-ms 100]\n"
    "                        [--quote-size 100] [--requote-ms 100] [--maker-fee-ticks 0]\n"
    "\n"
    "  --symbols A,B,C         replay several symbols in one pass, each into its own book\n"
    "  --all                   replay every symbol in the file\n"
    "  --utc-offset            exchange-local offset; default US Eastern for --date\n"
    "  --allow-truncated-tail  a file cut mid-message ends there instead of failing\n"
    "  --depth N               book levels to print per side (default 5)\n"
    "  --check-invariants N    check the book every N applied events (default 0 = only at the end)\n"
    "  --capacity N            order slots reserved for the book (default 1048576); with --symbols\n"
    "                          or --all, shared by every book (default 4194304)\n"
    "\n"
    "backtest runs a toy strategy against the replayed book, which our orders never change:\n"
    "  --strategy S            imbalance (default): IOCs on top-of-book imbalance;\n"
    "                          mm: post-only quotes at the best bid and ask\n"
    "  --order-latency-us N    strategy to exchange, in microseconds (default 50)\n"
    "  --report-latency-us N   exchange back to strategy and portfolio (default 50)\n"
    "  --max-position N        |position| limit in shares (default 500)\n"
    "  --max-open-orders N     risk limit on live orders (default 1 for imbalance, 2 for mm)\n"
    "  --taker-fee-ticks N     taker fee per share in price ticks, $0.0001 for ITCH (default 0)\n"
    "imbalance only:\n"
    "  --threshold X           trade when one top-of-book size >= X times the other (>= 1, default 3)\n"
    "  --clip N                shares per order (default 100)\n"
    "  --cooldown-ms N         wait after each order (default 100)\n"
    "mm only:\n"
    "  --quote-size N          shares per quote (default 100)\n"
    "  --requote-ms N          a quote lives at least this long before it is moved (default 100)\n"
    "  --maker-fee-ticks N     maker fee per share in price ticks; negative = rebate (default 0)\n";

enum class CliCommand : std::uint8_t { Replay, Backtest };

enum class StrategyKind : std::uint8_t { Imbalance, MarketMaker };

// backtest only.
struct BacktestArgs
{
    StrategyKind strategy{StrategyKind::Imbalance};
    std::uint64_t order_latency_us{50};
    std::uint64_t report_latency_us{50};
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

// The default --capacity of the order pool every book shares (--symbols, --all):
// 2^22, over twice the 1,924,078 live orders the 2019-12-30 full day peaks at,
// all symbols together. A full pool rejects Adds as PoolExhausted.
inline constexpr std::size_t default_books_capacity = 4'194'304;

struct ReplayArgs
{
    CliCommand command{CliCommand::Replay};
    std::string_view file;
    std::string_view symbol;                // 1-8 chars, an ITCH ticker
    std::vector<std::string_view> symbols;  // --symbols, each like symbol
    bool all_symbols{false};                // --all
    std::chrono::year_month_day date{};
    std::chrono::minutes utc_offset{0};  // --utc-offset, else us_eastern_utc_offset(date)
    bool allow_truncated_tail{false};
    std::size_t depth{5};
    std::uint64_t check_invariants_every{0};
    std::size_t capacity{1'048'576};
    BacktestArgs backtest{};
};

// `args` is argv without the program name: "replay" or "backtest", then the
// file and the flags in any order. Exactly one of --symbol, --symbols and
// --all; backtest takes only --symbol, and the backtest flags are rejected
// for replay. On false, `error` says what is wrong.
bool parse_replay_args(std::span<const std::string_view> args, ReplayArgs& out, std::string& error);

// "YYYY-MM-DD", a real calendar date.
bool parse_date(std::string_view text, std::chrono::year_month_day& date) noexcept;

// "+HH:MM" or "-HH:MM", HH < 24, MM < 60.
bool parse_utc_offset(std::string_view text, std::chrono::minutes& offset) noexcept;

} // namespace simulator
