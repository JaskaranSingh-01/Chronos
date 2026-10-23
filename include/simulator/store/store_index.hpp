#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace simulator {

// One symbol's day in the store: a byte range of the symbol's file holding its frames as zstd frames.
struct Segment
{
    std::uint64_t offset{0};
    std::uint64_t compressed{0};
    std::uint64_t uncompressed{0};
    std::uint64_t messages{0};
    std::uint64_t events{0};  // market events: A F E C X D U P
    std::string hash{};       // XXH3-128 of the uncompressed bytes, 32 lower-case hex digits

    bool operator==(const Segment&) const = default;
};

// A day built into the store: the day file it was read from, and its segments by symbol.
struct StoreDay
{
    std::string file{};  // e.g. 12302019.NASDAQ_ITCH50.bin
    std::uint64_t size{0};
    std::string hash{};  // XXH3-128 of the day file as the build read it
    std::map<std::string, Segment, std::less<>> segments{};

    bool operator==(const StoreDay&) const = default;
};

// The symbol's file name: the symbol with each byte outside [A-Z0-9.+=-] written %XX, then ".itch.zst".
std::string symbol_file(std::string_view symbol);
// The day file's name for `date`, e.g. 12302019.NASDAQ_ITCH50.bin.
std::string day_file(std::chrono::year_month_day date);
// Whether `text` is a hash as the index spells it: 32 lower-case hex digits.
bool is_hash(std::string_view text) noexcept;
// YYYY-MM-DD.
std::string iso_date(std::chrono::year_month_day date);

// The store's index.tsv (spec §2.1-§2.2): a `day` line per day, then a `segment` line per segment, tab-separated.
class StoreIndex
{
private:
    std::map<std::chrono::year_month_day, StoreDay> days_;

public:
    // Reads <dir>/index.tsv, none being an empty store, and checks its records against the symbol files. False, with
    // `error` naming the line or file, for any index spec §2.2 calls invalid.
    bool load(const std::filesystem::path& dir, std::string& error);
    std::string text() const;

    const std::map<std::chrono::year_month_day, StoreDay>& days() const noexcept
    {
        return days_;
    }

    // Adds `day`, or replaces a rebuilt day's records with it.
    void set(std::chrono::year_month_day date, StoreDay day);
    // The end of the symbol's last segment in its file; 0 without one.
    std::uint64_t committed_end(std::string_view symbol) const noexcept;

    bool operator==(const StoreIndex&) const = default;
};

}  // namespace simulator
