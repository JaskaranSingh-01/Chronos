#include "simulator/cli/replay_args.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

#include "simulator/types/timestamp.hpp"

namespace simulator {

namespace {

// All of `text` as a base-10 T; no spaces, and a '-' only for a signed T.
template <class T>
bool parse_number(std::string_view text, T& value) noexcept
{
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return !text.empty() && ec == std::errc{} && ptr == end;
}

bool parse_digits(std::string_view text, unsigned& value) noexcept
{
    return text.find_first_not_of("0123456789") == std::string_view::npos && parse_number(text, value);
}

// A finite decimal >= 1, all of `text`, e.g. "3" or "2.5".
bool parse_threshold(std::string_view text, double& value)
{
    if (text.empty() || text[0] < '0' || text[0] > '9') {  // no sign, space, nan or inf
        return false;
    }
    const std::string copy{text};
    char* end = nullptr;
    const double parsed = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(parsed) || parsed < 1.0) {
        return false;
    }
    value = parsed;
    return true;
}

// "A,B,C": each 1-8 chars, none empty.
bool parse_symbols(std::string_view text, std::vector<std::string_view>& symbols)
{
    for (std::size_t start = 0; start <= text.size();) {
        const std::size_t comma = std::min(text.find(',', start), text.size());
        const std::string_view symbol = text.substr(start, comma - start);
        if (symbol.empty() || symbol.size() > 8) {
            return false;
        }
        symbols.push_back(symbol);
        start = comma + 1;
    }
    return true;
}

// Cap on backtest numbers, so scaling to ns or multiplying by a price stays far from int64 overflow.
constexpr std::uint64_t max_value = 1'000'000'000;

// print_book allocates --depth levels per side.
constexpr std::size_t max_depth = 1'000;

// Beyond any machine's memory, and keeps OrderIndex's bit_ceil(2 * capacity) far from overflow.
constexpr std::size_t max_capacity = std::size_t{1} << 30;

// A latency in units of unit_ns as ns, at most 1,000 s; writes `ns` only on success.
bool parse_latency(std::string_view text, std::uint64_t unit_ns, std::uint64_t& ns) noexcept
{
    std::uint64_t value{0};
    if (!parse_number(text, value) || value > max_value * 1'000 / unit_ns) {
        return false;
    }
    ns = value * unit_ns;
    return true;
}

} // namespace

bool parse_date(std::string_view text, std::chrono::year_month_day& date) noexcept
{
    unsigned year{0};
    unsigned month{0};
    unsigned day{0};

    if (text.size() != 10 || text[4] != '-' || text[7] != '-' || !parse_digits(text.substr(0, 4), year) ||
        !parse_digits(text.substr(5, 2), month) || !parse_digits(text.substr(8, 2), day)) {
        return false;
    }

    const std::chrono::year_month_day parsed{std::chrono::year{static_cast<int>(year)}, std::chrono::month{month},
                                             std::chrono::day{day}};
    if (!parsed.ok()) {
        return false;
    }
    date = parsed;
    return true;
}

bool parse_utc_offset(std::string_view text, std::chrono::minutes& offset) noexcept
{
    unsigned hours{0};
    unsigned minutes{0};

    if (text.size() != 6 || (text[0] != '+' && text[0] != '-') || text[3] != ':' ||
        !parse_digits(text.substr(1, 2), hours) || !parse_digits(text.substr(4, 2), minutes) || hours >= 24 ||
        minutes >= 60) {
        return false;
    }

    const std::chrono::minutes magnitude{hours * 60 + minutes};
    offset = text[0] == '-' ? -magnitude : magnitude;
    return true;
}

bool parse_replay_args(std::span<const std::string_view> args, ReplayArgs& out, std::string& error)
{
    // The first help or version flag wins, even over a bad command or option.
    for (const std::string_view arg : args) {
        if (arg == "-h" || arg == "--help" || arg == "--version") {
            out = ReplayArgs{};
            out.command = arg == "--version" ? CliCommand::Version : CliCommand::Help;
            return true;
        }
    }

    if (args.empty()) {
        error = "missing the command: replay or backtest";
        return false;
    }
    if (args[0] != "replay" && args[0] != "backtest") {
        error = "unknown command '" + std::string{args[0]} + "': expected replay or backtest";
        return false;
    }

    ReplayArgs parsed;
    parsed.command = args[0] == "backtest" ? CliCommand::Backtest : CliCommand::Replay;
    const bool backtest = parsed.command == CliCommand::Backtest;
    BacktestArgs& b = parsed.backtest;
    bool has_date = false;
    bool has_offset = false;
    bool has_capacity = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];

        if (!arg.starts_with("--")) {
            if (!parsed.file.empty()) {
                error = "more than one file: '" + std::string{arg} + "'";
                return false;
            }
            parsed.file = arg;
            continue;
        }

        if (arg == "--allow-truncated-tail") {
            parsed.allow_truncated_tail = true;
            continue;
        }
        if (arg == "--all") {
            parsed.all_symbols = true;
            continue;
        }
        if (!backtest && arg == "--latency") {
            parsed.latency_batch = parsed.latency_batch != 0 ? parsed.latency_batch : default_latency_batch;
            continue;
        }

        // Checked after the option is known, so an unknown last option says so.
        const bool has_value = i + 1 < args.size();
        const std::string_view value = has_value ? args[++i] : std::string_view{};
        bool ok = true;

        if (arg == "--symbol") {
            parsed.symbol = value;
            ok = !value.empty() && value.size() <= 8;
        } else if (arg == "--symbols") {
            ok = parse_symbols(value, parsed.symbols);
        } else if (arg == "--date") {
            ok = has_date = parse_date(value, parsed.date);
        } else if (arg == "--store") {
            parsed.store = value;
            ok = !value.empty();
        } else if (arg == "--utc-offset") {
            ok = has_offset = parse_utc_offset(value, parsed.utc_offset);
        } else if (arg == "--depth") {
            ok = parse_number(value, parsed.depth) && parsed.depth <= max_depth;
        } else if (arg == "--check-invariants") {
            ok = parse_number(value, parsed.check_invariants_every);
        } else if (arg == "--capacity") {
            ok = has_capacity =
                parse_number(value, parsed.capacity) && parsed.capacity > 0 && parsed.capacity <= max_capacity;
        } else if (arg == "--threads") {
            ok = value == "1" || value == "2";
            parsed.threads = value == "2" ? 2 : 1;
        } else if (arg == "--book") {
            ok = value == "list" || value == "ladder";
            parsed.book = value == "ladder" ? BookBackend::Ladder : BookBackend::List;
        } else if (!backtest && arg == "--latency-batch") {
            ok = parse_number(value, parsed.latency_batch) && parsed.latency_batch > 0;
        } else if (arg == "--json") {
            parsed.json = value;
            ok = !value.empty();
        } else if (backtest && (arg == "--order-latency-ns" || arg == "--order-latency-us")) {
            ok = parse_latency(value, arg.ends_with("-us") ? 1'000 : 1, b.order_latency_ns);
        } else if (backtest && (arg == "--report-latency-ns" || arg == "--report-latency-us")) {
            ok = parse_latency(value, arg.ends_with("-us") ? 1'000 : 1, b.report_latency_ns);
        } else if (backtest && arg == "--threshold") {
            ok = parse_threshold(value, b.threshold);
        } else if (backtest && arg == "--clip") {
            ok = parse_number(value, b.clip) && b.clip > 0 && b.clip <= max_value;
        } else if (backtest && arg == "--max-position") {
            ok = parse_number(value, b.max_position) && b.max_position >= 0 &&
                static_cast<std::uint64_t>(b.max_position) <= max_value;
        } else if (backtest && arg == "--cooldown-ms") {
            ok = parse_number(value, b.cooldown_ms) && b.cooldown_ms <= max_value;
        } else if (backtest && arg == "--taker-fee-ticks") {
            ok = parse_number(value, b.taker_fee_ticks) && b.taker_fee_ticks <= 1'000'000;
        } else if (backtest && arg == "--strategy") {
            ok = value == "imbalance" || value == "mm";
            b.strategy = value == "mm" ? StrategyKind::MarketMaker : StrategyKind::Imbalance;
        } else if (backtest && arg == "--max-open-orders") {
            ok = parse_number(value, b.max_open_orders) && b.max_open_orders > 0 && b.max_open_orders <= max_value;
        } else if (backtest && arg == "--quote-size") {
            ok = parse_number(value, b.quote_size) && b.quote_size > 0 && b.quote_size <= max_value;
        } else if (backtest && arg == "--requote-ms") {
            ok = parse_number(value, b.requote_ms) && b.requote_ms <= max_value;
        } else if (backtest && arg == "--maker-fee-ticks") {
            ok = parse_number(value, b.maker_fee_ticks) && b.maker_fee_ticks >= -1'000'000 &&
                b.maker_fee_ticks <= 1'000'000;
        } else {
            error = "unknown " + std::string{args[0]} + " option " + std::string{arg};
            return false;
        }

        if (!has_value) {
            error = std::string{arg} + " needs a value";
            return false;
        }
        if (!ok) {
            error = "bad value for " + std::string{arg} + ": '" + std::string{value} + "'";
            return false;
        }
    }

    const bool many = !parsed.symbols.empty() || parsed.all_symbols;
    const int selections = !parsed.symbol.empty() + !parsed.symbols.empty() + parsed.all_symbols;

    if (!parsed.store.empty() && !parsed.file.empty()) {
        error = "--store replaces <itch-file>: give one";
    } else if (!parsed.store.empty() && many) {
        error = "--store reads one symbol: use --symbol";
    } else if (parsed.file.empty() && parsed.store.empty()) {
        error = "missing <itch-file>";
    } else if (selections == 0) {
        error = backtest ? "missing --symbol" : "missing --symbol (or --symbols A,B,C or --all)";
    } else if (selections > 1) {
        error = "--symbol, --symbols and --all are mutually exclusive";
    } else if (backtest && many) {
        error = "backtest runs one symbol: use --symbol, not --symbols or --all";
    } else if (!has_date) {
        error = "missing --date";
    } else {
        if (!has_offset) {
            parsed.utc_offset = us_eastern_utc_offset(parsed.date);
        }
        if (many && !has_capacity) {
            parsed.capacity = default_books_capacity;
        }
        out = parsed;
        return true;
    }
    return false;
}

} // namespace simulator
