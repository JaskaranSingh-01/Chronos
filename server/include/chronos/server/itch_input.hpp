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
#include "simulator/market/instrument_registry.hpp"
#include "simulator/store/store.hpp"

namespace simulator::server {

// FNV-1a 64 of the first and last MiB: a day's quick check.
// ponytail: an edit between them at the same size goes unseen; day_hash() of the whole file sees it.
std::uint64_t quick_check(std::span<const std::byte> bytes) noexcept;

// XXH3-128 of every byte, as 32 hex digits: a day's identity.
std::string day_hash(std::span<const std::byte> bytes);

// XXH3-128 of the segment hashes in order, as 32 hex digits: the inputs hash of a run, which a Watch compares.
std::string inputs_hash(std::span<const std::string> hashes);

// YYYY-MM-DD.
std::string iso_date(std::chrono::year_month_day date);

// One day file of a DayLibrary, identified by the one pass over its bytes that builds it into the store.
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
    std::vector<std::string> symbols{};  // those with a market event that day, sorted
    Timestamp midnight{0};               // session_midnight_ns() in New York
    std::filesystem::path store{};       // where its segments are
    std::map<std::string, Segment, std::less<>> segments{};  // as the store's index listed them, by symbol

    // Its segment of `symbol`; nullptr if the store holds none.
    const Segment* segment(std::string_view symbol) const;
};

// A generation or a run keeps its days, and so their segments as listed, through these, whatever a rescan finds later.
using DayHandle = std::shared_ptr<const Day>;

// The hash of `symbol`'s segment of `day` if it reads back intact now; "" if it does not, or there is none.
std::string segment_hash_as_read(const Day& day, std::string_view symbol);

// An exclusive flock on <store>/.lock while it lives, so that one chronos-serve at a time builds into a store.
class StoreLock
{
private:
    int fd_{-1};
    std::string error_;

public:
    explicit StoreLock(const std::filesystem::path& store);
    ~StoreLock();

    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;

    // "" while it is held; else why not, "in use by another chronos-serve" if another process holds it.
    const std::string& error() const noexcept
    {
        return error_;
    }
};

// The days of a directory: each file named exactly MMDDYYYY.NASDAQ_ITCH50.bin with a real date, whose store is current.
// A new file version (name, size, mtime, inode, or a changed quick check) is current if the JSON cache at `cache`
// holds its hash and the store a day of that hash; else one pass over its bytes hashes it and builds it into the
// store (spec §2.5). Any thread; a pass runs outside the lock on the days.
class DayLibrary
{
private:
    std::filesystem::path dir_;
    std::filesystem::path cache_;
    std::filesystem::path store_;
    // One scan, and so one build, at a time: no file is read twice, and the cache and the store have one writer.
    std::mutex scan_mutex_;
    StoreIndex index_;  // the store's; under scan_mutex_
    mutable std::mutex mutex_;
    std::map<std::chrono::year_month_day, DayHandle> days_;

    bool current(const Day& day) const;
    bool build(const std::filesystem::path& path, Day& day);

public:
    // The store is `store`, or <dir>/store. An index that is invalid is moved aside as index.tsv.bad and the symbol
    // files removed, so that every day is built again; stderr says so (spec §2.2).
    DayLibrary(std::filesystem::path dir, std::filesystem::path cache, std::filesystem::path store = {});

    // Lists the directory again, reading each new version with a line on stderr; the days, oldest first. A file it
    // cannot read whole, or whose store build fails, is left out; a directory it cannot list leaves the days, and the
    // cache, as they were.
    std::vector<DayHandle> scan();
    // The day the last scan found for `date`; nullptr if none.
    DayHandle find(std::chrono::year_month_day date) const;
    // The store's symbol files, and their bytes no segment holds.
    StoreUsage usage();
};

// Each generation's stream: a DaySequenceSource of `ticker` over `days` from `library`, or for the empty ticker an
// empty stream; for a Watch (`watch`), with hash as read. The engine thread resolves the days only here, as it builds a
// generation. The library outlives it.
// ponytail: a day removed between admission and the load replays as an empty stream, and one replaced by a non-ITCH
// file fails the chain up front, named as its first day (a Watch's verdict is then mismatch, not input_changed);
// re-check the days here if that matters.
SourceFactory chain_factory(const DayLibrary& library, const Instrument& instrument, bool allow_truncated_tail,
                            bool watch = false);

// One ticker over days in date order: each day's segment, read whole as the day begins, under its own ItchSource with
// the ticker SymbolId 0, and a Clear at each next day's midnight. A day not ITCH 5.0 (InvalidHeader), with other
// instrument fields (InstrumentRejected) or whose segment does not read back intact (StoreRead, said on stderr) fails
// the chain. With `hash_as_read`, it keeps each segment's hash as read.
// ponytail: a day's whole segment is in memory, 136 MiB for the busiest symbol-day measured; stream it from its zstd
// frames if a symbol-day ever passes about 1 GiB.
class DaySequenceSource final : public MarketEventSource
{
private:
    std::vector<DayHandle> days_;
    std::string ticker_;
    Instrument instrument_;
    bool allow_truncated_tail_;
    bool hash_as_read_;
    std::size_t day_{0};  // the day being read
    std::unique_ptr<MarketEventSource> source_;  // its source; null between days
    std::uint64_t before_{0};  // the sizes of the segments before it
    std::vector<std::string> hashes_;
    SourceError error_{SourceError::None};

public:
    DaySequenceSource(std::vector<DayHandle> days, std::string_view ticker, const Instrument& instrument,
                      bool allow_truncated_tail, bool hash_as_read);

    SourceResult next(MarketEvent& event) noexcept override;
    // The sizes of the segments before the current one, plus its own position.
    std::uint64_t position() const noexcept override;
    // The chain's: a day refused up front, or the current day's.
    SourceError error() const noexcept override;

    // position() at the end: the segments' sizes.
    std::uint64_t size() const noexcept;
    const std::vector<DayHandle>& days() const noexcept;
    // With hash as read, the hash of each day's segment as it was read, in order; "" for one that did not read back.
    const std::vector<std::string>& hashes() const noexcept;
};

}  // namespace simulator::server
