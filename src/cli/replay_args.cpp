#include "simulator/cli/replay_args.hpp"

#include <charconv>

#include "simulator/types/timestamp.hpp"

namespace simulator {

namespace {

// All of `text` as a base-10 unsigned number; no sign, no spaces.
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

} // namespace

bool parse_date(std::string_view text, std::chrono::year_month_day& date) noexcept
{
    unsigned year{0};
    unsigned month{0};
    unsigned day{0};

    if (text.size() != 10 || text[4] != '-' || text[7] != '-' ||
        !parse_digits(text.substr(0, 4), year) || !parse_digits(text.substr(5, 2), month) ||
        !parse_digits(text.substr(8, 2), day)) {
        return false;
    }

    const std::chrono::year_month_day parsed{
        std::chrono::year{static_cast<int>(year)}, std::chrono::month{month}, std::chrono::day{day}};
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
        !parse_digits(text.substr(1, 2), hours) || !parse_digits(text.substr(4, 2), minutes) ||
        hours >= 24 || minutes >= 60) {
        return false;
    }

    const std::chrono::minutes magnitude{hours * 60 + minutes};
    offset = text[0] == '-' ? -magnitude : magnitude;
    return true;
}

bool parse_replay_args(std::span<const std::string_view> args, ReplayArgs& out, std::string& error)
{
    if (args.empty() || args[0] != "replay") {
        error = "expected the command 'replay'";
        return false;
    }

    ReplayArgs parsed;
    bool has_date = false;
    bool has_offset = false;

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

        if (i + 1 == args.size()) {
            error = std::string{arg} + " needs a value";
            return false;
        }
        const std::string_view value = args[++i];
        bool ok = true;

        if (arg == "--symbol") {
            parsed.symbol = value;
            ok = !value.empty() && value.size() <= 8;
        } else if (arg == "--date") {
            ok = has_date = parse_date(value, parsed.date);
        } else if (arg == "--utc-offset") {
            ok = has_offset = parse_utc_offset(value, parsed.utc_offset);
        } else if (arg == "--depth") {
            ok = parse_number(value, parsed.depth);
        } else if (arg == "--check-invariants") {
            ok = parse_number(value, parsed.check_invariants_every);
        } else if (arg == "--capacity") {
            ok = parse_number(value, parsed.capacity) && parsed.capacity > 0;
        } else {
            error = "unknown option " + std::string{arg};
            return false;
        }

        if (!ok) {
            error = "bad value for " + std::string{arg} + ": '" + std::string{value} + "'";
            return false;
        }
    }

    if (parsed.file.empty()) {
        error = "missing <itch-file>";
    } else if (parsed.symbol.empty()) {
        error = "missing --symbol";
    } else if (!has_date) {
        error = "missing --date";
    } else {
        if (!has_offset) {
            parsed.utc_offset = us_eastern_utc_offset(parsed.date);
        }
        out = parsed;
        return true;
    }
    return false;
}

} // namespace simulator
