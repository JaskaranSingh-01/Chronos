#include "simulator/store/store_index.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#include "simulator/cli/replay_args.hpp"

namespace simulator {
namespace {

bool plain(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '=' || c == '-';
}

std::string encode(std::string_view symbol)
{
    std::string text;
    for (const char c : symbol) {
        char escaped[4];
        std::snprintf(escaped, sizeof escaped, "%%%02X", static_cast<unsigned>(static_cast<unsigned char>(c)));
        text += plain(c) ? std::string{c} : std::string{escaped};
    }
    return text;
}

// The 1-8 byte symbol `text` encodes; false unless `text` is exactly encode() of it.
bool decode(std::string_view text, std::string& symbol)
{
    symbol.clear();
    for (std::size_t i = 0; i < text.size(); ++i) {
        unsigned byte = static_cast<unsigned char>(text[i]);
        if (text[i] == '%') {
            const char* end = text.data() + std::min(i + 3, text.size());
            if (std::from_chars(text.data() + i + 1, end, byte, 16).ptr != end || end != text.data() + i + 3) {
                return false;
            }
            i += 2;
        }
        symbol += static_cast<char>(byte);
    }
    return !symbol.empty() && symbol.size() <= 8 && symbol.back() != ' ' && encode(symbol) == text;
}

bool number(std::string_view text, std::uint64_t& value) noexcept
{
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return !text.empty() && ec == std::errc{} && ptr == end && (text.size() == 1 || text[0] != '0');  // one spelling
}

std::vector<std::string_view> fields(std::string_view line)
{
    std::vector<std::string_view> split;
    for (std::size_t at = 0;;) {
        const std::size_t tab = line.find('\t', at);
        split.push_back(line.substr(at, tab - at));
        if (tab == std::string_view::npos) {
            return split;
        }
        at = tab + 1;
    }
}

// 0, with its size, if `path` is a regular file; else the errno of its stat, or -1 for another kind of file.
int regular(const std::filesystem::path& path, std::uint64_t& size) noexcept
{
    struct stat status{};
    if (::stat(path.c_str(), &status) != 0) {
        return errno;
    }
    size = static_cast<std::uint64_t>(status.st_size);
    return S_ISREG(status.st_mode) ? 0 : -1;
}

std::string why(int error)
{
    return error < 0 ? "not a file" : std::strerror(error);
}

}  // namespace

std::string symbol_file(std::string_view symbol)
{
    return encode(symbol) + ".itch.zst";
}

std::string day_file(std::chrono::year_month_day date)
{
    char text[32];
    std::snprintf(text, sizeof text, "%02u%02u%04d.NASDAQ_ITCH50.bin", unsigned{date.month()}, unsigned{date.day()},
                  int{date.year()});
    return text;
}

bool is_hash(std::string_view text) noexcept
{
    return text.size() == 32 &&
        std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string iso_date(std::chrono::year_month_day date)
{
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", int{date.year()}, unsigned{date.month()}, unsigned{date.day()});
    return text;
}

bool StoreIndex::load(const std::filesystem::path& dir, std::string& error)
{
    days_.clear();
    const auto fail = [&](std::string message) {
        error = std::move(message);
        return false;
    };
    std::uint64_t size = 0;
    if (const int e = regular(dir / "index.tsv", size); e == ENOENT) {
        return true;
    } else if (e != 0) {
        return fail("index.tsv: " + why(e));
    }
    std::ifstream in{dir / "index.tsv", std::ios::binary};
    const std::string text{std::istreambuf_iterator<char>{in}, {}};
    if (!in.is_open() || text.size() != size) {
        return fail("index.tsv: cannot be read");
    }
    std::map<std::chrono::year_month_day, StoreDay> days;
    std::size_t line = 0;
    for (std::size_t at = 0; at < text.size();) {
        const std::string where = "line " + std::to_string(++line) + ": ";
        const std::size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            return fail(where + "no end of line");
        }
        const std::vector<std::string_view> f = fields(std::string_view{text}.substr(at, end - at));
        at = end + 1;
        const bool day = f.size() == 5 && f[0] == "day";
        std::chrono::year_month_day date{};
        if (!day && (f.size() != 9 || f[0] != "segment")) {
            return fail(where + "not a record");
        }
        if (!parse_date(f[1], date)) {
            return fail(where + "bad date");
        }
        if (day) {
            StoreDay record{.file = std::string{f[2]}, .hash = std::string{f[4]}};
            if (record.file != day_file(date)) {
                return fail(where + "bad file");
            }
            if (!number(f[3], record.size)) {
                return fail(where + "bad number");
            }
            if (!is_hash(f[4])) {
                return fail(where + "bad hash");
            }
            if (!days.emplace(date, std::move(record)).second) {
                return fail(where + "day repeated");
            }
            continue;
        }
        std::string symbol;
        Segment s{.hash = std::string{f[8]}};
        const auto found = days.find(date);
        if (!decode(f[2], symbol)) {
            return fail(where + "bad symbol");
        }
        if (!number(f[3], s.offset) || !number(f[4], s.compressed) ||
            s.compressed > std::numeric_limits<std::uint64_t>::max() - s.offset || !number(f[5], s.uncompressed) ||
            !number(f[6], s.messages) || !number(f[7], s.events)) {
            return fail(where + "bad number");
        }
        if (!is_hash(f[8])) {
            return fail(where + "bad hash");
        }
        if (found == days.end()) {
            return fail(where + "segment without its day");
        }
        if (!found->second.segments.emplace(std::move(symbol), std::move(s)).second) {
            return fail(where + "segment repeated");
        }
    }
    std::map<std::string, std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges;  // each file's [start, end)
    for (const auto& [date, day] : days) {
        for (const auto& [symbol, s] : day.segments) {
            ranges[symbol].emplace_back(s.offset, s.offset + s.compressed);
        }
    }
    for (auto& [symbol, list] : ranges) {
        const std::string file = symbol_file(symbol);
        std::sort(list.begin(), list.end());
        if (const int e = regular(dir / file, size); e != 0) {
            return fail(file + ": " + why(e));
        }
        if (std::adjacent_find(list.begin(), list.end(),
                               [](const auto& a, const auto& b) { return a.second > b.first; }) != list.end()) {
            return fail(file + ": overlapping segments");
        }
        if (list.back().second > size) {
            return fail(file + ": shorter than its segments");
        }
    }
    days_ = std::move(days);
    return true;
}

std::string StoreIndex::text() const
{
    std::string text;
    for (const auto& [date, day] : days_) {
        text += "day\t" + iso_date(date) + '\t' + day.file + '\t' + std::to_string(day.size) + '\t' + day.hash + '\n';
    }
    for (const auto& [date, day] : days_) {
        for (const auto& [symbol, s] : day.segments) {
            text += "segment\t" + iso_date(date) + '\t' + encode(symbol) + '\t' + std::to_string(s.offset) + '\t' +
                std::to_string(s.compressed) + '\t' + std::to_string(s.uncompressed) + '\t' +
                std::to_string(s.messages) + '\t' + std::to_string(s.events) + '\t' + s.hash + '\n';
        }
    }
    return text;
}

void StoreIndex::set(std::chrono::year_month_day date, StoreDay day)
{
    days_.insert_or_assign(date, std::move(day));
}

std::uint64_t StoreIndex::committed_end(std::string_view symbol) const noexcept
{
    std::uint64_t end = 0;
    for (const auto& [date, day] : days_) {
        if (const auto found = day.segments.find(symbol); found != day.segments.end()) {
            end = std::max(end, found->second.offset + found->second.compressed);
        }
    }
    return end;
}

}  // namespace simulator
