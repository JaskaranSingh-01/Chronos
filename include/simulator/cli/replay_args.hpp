#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace simulator {

inline constexpr std::string_view replay_usage =
    "usage: simulator replay <itch-file> --symbol AAPL --date 2019-12-30\n"
    "                        [--utc-offset -05:00] [--allow-truncated-tail] [--depth 5]\n"
    "                        [--check-invariants N] [--capacity N]\n"
    "\n"
    "  --utc-offset            exchange-local offset; default US Eastern for --date\n"
    "  --allow-truncated-tail  a file cut mid-message ends there instead of failing\n"
    "  --depth N               book levels to print per side (default 5)\n"
    "  --check-invariants N    check the book every N applied events (default 0 = only at the end)\n"
    "  --capacity N            order slots reserved for the book (default 1048576)\n";

struct ReplayArgs
{
    std::string_view file;
    std::string_view symbol;  // 1-8 chars, an ITCH ticker
    std::chrono::year_month_day date{};
    std::chrono::minutes utc_offset{0};  // --utc-offset, else us_eastern_utc_offset(date)
    bool allow_truncated_tail{false};
    std::size_t depth{5};
    std::uint64_t check_invariants_every{0};
    std::size_t capacity{1'048'576};
};

// `args` is argv without the program name: "replay", the file and the flags,
// in any order after "replay". On false, `error` says what is wrong.
bool parse_replay_args(std::span<const std::string_view> args, ReplayArgs& out, std::string& error);

// "YYYY-MM-DD", a real calendar date.
bool parse_date(std::string_view text, std::chrono::year_month_day& date) noexcept;

// "+HH:MM" or "-HH:MM", HH < 24, MM < 60.
bool parse_utc_offset(std::string_view text, std::chrono::minutes& offset) noexcept;

} // namespace simulator
