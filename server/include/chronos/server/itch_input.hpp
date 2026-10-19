#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "chronos/server/generation.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/market/instrument_registry.hpp"

namespace simulator::server {

// The tickers of the stock directory (R) messages before the start of system hours (S 'S'), sorted.
// ponytail: an R after the start of system hours (an intraday listing) is left out; scan the whole day if one matters.
std::vector<std::string> symbol_directory(std::span<const std::byte> bytes);

// FNV-1a 64 of the first and last MiB: a day's quick check.
// ponytail: an edit between them at the same size goes unseen; day_hash() of the whole file sees it.
std::uint64_t quick_check(std::span<const std::byte> bytes) noexcept;

// Names the market data in the command log: "<name> <size in bytes> <quick_check(), hex>".
std::string input_fingerprint(std::string_view name, std::span<const std::byte> bytes);

// XXH3-128 of every byte, as 32 hex digits: a day's identity.
std::string day_hash(std::span<const std::byte> bytes);

// YYYY-MM-DD.
std::string iso_date(std::chrono::year_month_day date);

// One ITCH day's streams: per ticker, an ItchSource with its own one-instrument registry; for the empty ticker, an
// empty stream. Never throws but for memory. The caller keeps `bytes` alive.
SourceFactory itch_factory(std::span<const std::byte> bytes, std::chrono::year_month_day date,
                           std::chrono::minutes utc_offset, bool allow_truncated_tail);

enum class SessionSource : std::uint8_t {
    Feed,     // the day's System Events 'Q' (start of market hours) and 'M' (end of market hours)
    Fallback  // either is missing: 09:30-16:00 ET
};

// A day's regular hours, [open, close) in epoch ns.
struct FeedSession
{
    Timestamp open{0};
    Timestamp close{0};
    SessionSource source{SessionSource::Fallback};

    bool operator==(const FeedSession&) const = default;
};

// One day file of a DayLibrary, identified by one pass over its bytes.
struct Day
{
    std::chrono::year_month_day date{};
    std::string file{};  // the name, e.g. 12302019.NASDAQ_ITCH50.bin
    std::uint64_t size{0};
    std::int64_t mtime{0};  // the file clock's ns
    std::uint64_t inode{0};
    std::string hash{};  // day_hash()
    std::uint64_t quick_check{0};
    bool itch50{false};    // the first message is the System Event 'O', and the framing holds to the end
    bool cut_tail{false};  // the framing holds to a last message that is cut off
    FeedSession session{};
    std::vector<std::string> symbols{};  // symbol_directory()
    Timestamp midnight{0};               // session_midnight_ns() in New York
    MappedFile mapped{};
};

// A generation or a run keeps its days mapped through these, whatever a rescan finds later.
using DayHandle = std::shared_ptr<const Day>;

// The days of a directory: each file named exactly MMDDYYYY.NASDAQ_ITCH50.bin with a real date. A new file version
// (name, size, mtime, inode, or a changed quick check) gets one pass over its bytes, unless the JSON cache at `cache`
// holds it. Any thread; mapping and hashing run outside the lock on the days.
class DayLibrary
{
private:
    std::filesystem::path dir_;
    std::filesystem::path cache_;
    std::mutex scan_mutex_;  // one scan at a time, so no file is hashed twice and the cache has one writer
    mutable std::mutex mutex_;
    std::map<std::chrono::year_month_day, DayHandle> days_;

public:
    DayLibrary(std::filesystem::path dir, std::filesystem::path cache);

    // Lists the directory again, hashing each new version with a line on stderr; the days, oldest first. A file it
    // cannot read whole is left out; a directory it cannot list leaves the days, and the cache, as they were.
    std::vector<DayHandle> scan();
    // The day the last scan found for `date`; nullptr if none.
    DayHandle find(std::chrono::year_month_day date) const;
};

// One ticker over days in date order, the ticker SymbolId 0 in each day's own ItchSource, with a Clear at each next
// day's midnight. A day not ITCH 5.0 (InvalidHeader) or with other instrument fields (InstrumentRejected) fails the
// chain. With `hash_as_read`, each day's bytes go into XXH3-128 as replayed, and the rest of the file as it ends.
class DaySequenceSource final : public MarketEventSource
{
private:
    struct Hashing;
    std::vector<DayHandle> days_;
    std::string ticker_;
    Instrument instrument_;
    bool allow_truncated_tail_;
    std::size_t day_{0};                         // the day being read
    std::unique_ptr<MarketEventSource> source_;  // its source; null between days
    std::uint64_t before_{0};                    // the sizes of the days before it
    std::unique_ptr<Hashing> hashing_;           // null without hash as read
    std::vector<std::string> hashes_;
    SourceError error_{SourceError::None};

public:
    DaySequenceSource(std::vector<DayHandle> days, std::string_view ticker, const Instrument& instrument,
                      bool allow_truncated_tail, bool hash_as_read);
    ~DaySequenceSource() override;

    SourceResult next(MarketEvent& event) noexcept override;
    // The sizes of the days before the current one, plus its own position.
    std::uint64_t position() const noexcept override;
    // The chain's: a day refused up front, or the current day's.
    SourceError error() const noexcept override;

    // position() at the end: the days' sizes.
    std::uint64_t size() const noexcept;
    // With hash as read, day_hash() of each day replayed to its end, in order.
    const std::vector<std::string>& hashes() const noexcept;
};

}  // namespace simulator::server
