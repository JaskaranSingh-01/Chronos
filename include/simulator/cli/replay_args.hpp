#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace simulator {

inline constexpr std::string_view usage =
    "usage: simulator replay <itch-file> --symbol AAPL --date 2019-12-30\n"
    "                        [--utc-offset -05:00] [--allow-truncated-tail] [--depth 5]\n"
    "                        [--check-invariants N] [--capacity N]\n"
    "       simulator backtest <itch-file> --symbol AAPL --date 2019-12-30 [replay options]\n"
    "                        [--order-latency-us 50] [--report-latency-us 50] [--threshold 3]\n"
    "                        [--clip 100] [--max-position 500] [--cooldown-ms 100] [--taker-fee-ticks 0]\n"
    "\n"
    "  --utc-offset            exchange-local offset; default US Eastern for --date\n"
    "  --allow-truncated-tail  a file cut mid-message ends there instead of failing\n"
    "  --depth N               book levels to print per side (default 5)\n"
    "  --check-invariants N    check the book every N applied events (default 0 = only at the end)\n"
    "  --capacity N            order slots reserved for the book (default 1048576)\n"
    "\n"
    "backtest runs the ImbalanceTaker toy strategy (IOC only) against the replayed book:\n"
    "  --order-latency-us N    strategy to exchange, in microseconds (default 50)\n"
    "  --report-latency-us N   exchange back to strategy and portfolio (default 50)\n"
    "  --threshold X           trade when one top-of-book size >= X times the other (>= 1, default 3)\n"
    "  --clip N                shares per order (default 100)\n"
    "  --max-position N        |position| limit in shares (default 500)\n"
    "  --cooldown-ms N         wait after each order (default 100)\n"
    "  --taker-fee-ticks N     taker fee per share in price ticks, $0.0001 for ITCH (default 0)\n";

enum class CliCommand : std::uint8_t { Replay, Backtest };

// backtest only.
struct BacktestArgs
{
    std::uint64_t order_latency_us{50};
    std::uint64_t report_latency_us{50};
    double threshold{3.0};
    std::uint64_t clip{100};
    std::int64_t max_position{500};
    std::uint64_t cooldown_ms{100};
    std::uint64_t taker_fee_ticks{0};
};

struct ReplayArgs
{
    CliCommand command{CliCommand::Replay};
    std::string_view file;
    std::string_view symbol;  // 1-8 chars, an ITCH ticker
    std::chrono::year_month_day date{};
    std::chrono::minutes utc_offset{0};  // --utc-offset, else us_eastern_utc_offset(date)
    bool allow_truncated_tail{false};
    std::size_t depth{5};
    std::uint64_t check_invariants_every{0};
    std::size_t capacity{1'048'576};
    BacktestArgs backtest{};
};

// `args` is argv without the program name: "replay" or "backtest", then the
// file and the flags in any order. The backtest flags are rejected for
// replay. On false, `error` says what is wrong.
bool parse_replay_args(std::span<const std::string_view> args, ReplayArgs& out, std::string& error);

// "YYYY-MM-DD", a real calendar date.
bool parse_date(std::string_view text, std::chrono::year_month_day& date) noexcept;

// "+HH:MM" or "-HH:MM", HH < 24, MM < 60.
bool parse_utc_offset(std::string_view text, std::chrono::minutes& offset) noexcept;

} // namespace simulator
