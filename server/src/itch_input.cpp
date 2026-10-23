#include "chronos/server/itch_input.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "json_enums.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/itch/byte_reader.hpp"
#include "simulator/itch/itch_message_reader.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_book.hpp"
#include "simulator/market/vector_source.hpp"

namespace simulator::server {
namespace {

// One symbol's segment of a day under an ItchSource that owns the bytes and the registry it registers the symbol in.
class StoreSource final : public MarketEventSource
{
private:
    std::vector<std::byte> bytes_;
    InstrumentRegistry registry_{1};
    ItchSource source_;

public:
    StoreSource(std::vector<std::byte> bytes, const ItchSourceConfig& config)
        : bytes_(std::move(bytes)), source_(bytes_, config, registry_)
    {}

    SourceResult next(MarketEvent& event) noexcept override
    {
        return source_.next(event);
    }

    std::uint64_t position() const noexcept override
    {
        return source_.position();
    }

    SourceError error() const noexcept override
    {
        return source_.error();
    }

    const Instrument& instrument() const noexcept
    {
        return source_.instrument();
    }
};

std::string hex(std::uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof text, "%016" PRIx64, value);
    return text;
}

// As xxhsum prints it: the high half first.
std::string hex(XXH128_hash_t hash)
{
    return hex(hash.high64) + hex(hash.low64);
}

// The date a day file's name gives, e.g. 12302019.NASDAQ_ITCH50.bin; false for any other name or no real date.
bool day_date(std::string_view name, std::chrono::year_month_day& date) noexcept
{
    constexpr std::string_view suffix = ".NASDAQ_ITCH50.bin";
    if (name.size() != 8 + suffix.size() || !name.ends_with(suffix) ||
        !std::all_of(name.begin(), name.begin() + 8, [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    const auto number = [&](std::size_t at, std::size_t digits) {
        unsigned value = 0;
        for (std::size_t i = at; i < at + digits; ++i) {
            value = value * 10 + static_cast<unsigned>(name[i] - '0');
        }
        return value;
    };
    const unsigned year = number(4, 4);
    if (year < 1970 || year > 2261) {  // session_midnight_ns() stays within int64 ns
        return false;
    }
    date =
        std::chrono::year{static_cast<int>(year)} / std::chrono::month{number(0, 2)} / std::chrono::day{number(2, 2)};
    return date.ok();
}

// A pass over a new file version: its hash, its feed session and its ITCH 5.0 check, and its frames fed to `builder`
// if not null. It reads the file a MiB at a time, at disk speed, where faulting a mapping in is several times slower.
// False, said on stderr, unless it read exactly the size the version was listed with.
bool identify(const std::filesystem::path& path, Day& day, StoreBuilder* builder)
{
    const auto feed = [builder](std::span<const std::byte> frame) {
        if (builder != nullptr) {
            builder->add(frame);
        }
    };
    constexpr std::size_t step = std::size_t{1} << 20;
    constexpr std::size_t max_frame = 2 + 0xFFFF;  // the length prefix and the largest ITCH payload
    std::vector<std::byte> buffer(max_frame + step);  // the frame the last step cut, then a step
    day.itch50 = false;  // not the cache's
    std::size_t carry = 0;
    std::uint64_t hashed = 0;
    XXH3_state_t state;
    XXH3_128bits_reset(&state);
    bool first = true;
    bool framed = true;
    std::optional<Timestamp> open;
    std::optional<Timestamp> close;
    std::ifstream in{path, std::ios::binary};
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data() + carry), step);
        const auto got = static_cast<std::size_t>(in.gcount());
        hashed += got;
        XXH3_128bits_update(&state, buffer.data() + carry, got);
        ItchMessageReader reader{std::span{buffer}.first(carry + got)};
        ItchMessage message;
        for (std::size_t from = 0; framed && reader.next(message) == ItchReadResult::Message;
             from = reader.position()) {
            feed(std::span{buffer}.subspan(from, reader.position() - from));
            const bool start = std::exchange(first, false);
            if (message.type != 'S' || message.payload.size() != 11) {  // a System Event: locate, tracking, ts, code
                continue;
            }
            const auto code = std::to_integer<char>(message.payload[10]);
            std::uint64_t since_midnight = 0;
            ByteReader{message.payload.subspan(4, 6)}.read_u48_be(since_midnight);
            const Timestamp at = day.midnight + static_cast<Timestamp>(since_midnight);
            day.itch50 = day.itch50 || (start && code == 'O');
            if (code == 'Q') {
                open = at;
            } else if (code == 'M') {
                close = at;
            }
        }
        if (framed && reader.error() == ItchReadError::InvalidLength) {  // else a short frame: more to read
            framed = false;
            feed(std::span{buffer}.subspan(reader.position(), 2));  // the zero length: every segment fails at it
        }
        carry = framed ? reader.remaining() : 0;
        std::memmove(buffer.data(), buffer.data() + reader.position(), carry);
    }
    if (in.bad() || !in.eof() || hashed != day.size) {
        std::fprintf(stderr, "skipping %s: read %" PRIu64 " of %" PRIu64 " bytes\n", day.file.c_str(), hashed,
                     day.size);
        return false;
    }
    day.cut_tail = framed && carry > 0;
    if (day.cut_tail) {
        feed(std::span{buffer}.first(carry));
    }
    day.itch50 = day.itch50 && framed;
    day.hash = hex(XXH3_128bits_digest(&state));
    day.session = open && close && *open < *close
        ? FeedSession{.open = *open, .close = *close, .source = SessionSource::Feed}
        : FeedSession{.open = day.midnight + RTH_OPEN_NS, .close = day.midnight + RTH_CLOSE_NS};
    return true;
}

// Reads a day file's version: its date from its name, its size, its mtime and inode and its quick check. False if the
// name is not a day file's, or the file cannot be mapped or dated (said on stderr).
bool open_version(const std::filesystem::path& path, Day& day)
{
    day.file = path.filename().string();
    struct stat status{};
    if (!day_date(day.file, day.date) || ::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return false;
    }
    // ponytail: a file truncated in place while its quick check reads the mapping can SIGBUS; pread() the two MiB if
    // that matters.
    MappedFile mapped;
    if (!mapped.open(path.c_str())) {
        std::fprintf(stderr, "skipping %s: %s\n", day.file.c_str(), mapped.error());
        return false;
    }
    std::error_code error;
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(path, error);
    if (error) {
        std::fprintf(stderr, "skipping %s: %s\n", day.file.c_str(), error.message().c_str());
        return false;
    }
    day.size = mapped.bytes().size();
    day.inode = static_cast<std::uint64_t>(status.st_ino);
    day.mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(written.time_since_epoch()).count();
    day.quick_check = quick_check(mapped.bytes());
    return true;
}

bool same_version(const Day& a, const Day& b) noexcept
{
    return a.file == b.file && a.size == b.size && a.mtime == b.mtime && a.inode == b.inode &&
        a.quick_check == b.quick_check;
}

// The cache file's JSON; discarded if it is missing, unreadable or does not parse.
nlohmann::json read_cache(const std::filesystem::path& path)
{
    try {
        return nlohmann::json::parse(std::ifstream{path}, nullptr, false);
    } catch (const std::ios_base::failure&) {  // libstdc++ throws on a read error, such as a directory
        return nlohmann::json::value_t::discarded;
    }
}

// Takes `day`'s hash, ITCH check and session from the cache entry of its version; false if there is none.
bool from_cache(const nlohmann::json& cache, Day& day)
{
    try {
        for (const nlohmann::json& entry : cache.at("days")) {
            if (entry.at("file") == day.file && entry.at("size") == day.size && entry.at("mtime") == day.mtime &&
                entry.at("inode") == day.inode && entry.at("quick_check") == hex(day.quick_check)) {
                auto hash = entry.at("hash").get<std::string>();
                const auto itch50 = entry.at("itch50").get<bool>();
                const auto cut_tail = entry.at("cut_tail").get<bool>();
                const auto session = entry.at("session").get<FeedSession>();
                day.hash = std::move(hash);  // only now, with every field parsed
                day.itch50 = itch50;
                day.cut_tail = cut_tail;
                day.session = session;
                return true;
            }
        }
    } catch (const nlohmann::json::exception&) {  // an unreadable cache: every day is hashed again
    }
    return false;
}

// Writes every day's version through a temp file and a rename; a failure keeps the hashes in memory only.
void write_cache(const std::filesystem::path& path, const std::map<std::chrono::year_month_day, DayHandle>& days)
{
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [date, day] : days) {
        entries.push_back({{"file", day->file},
                           {"size", day->size},
                           {"mtime", day->mtime},
                           {"inode", day->inode},
                           {"quick_check", hex(day->quick_check)},
                           {"hash", day->hash},
                           {"itch50", day->itch50},
                           {"cut_tail", day->cut_tail},
                           {"session", day->session}});
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const std::filesystem::path temp = path.string() + ".tmp";
    std::ofstream out{temp};
    out << nlohmann::json{{"days", entries}}.dump() << '\n';
    out.close();
    if (out) {
        std::filesystem::rename(temp, path, error);
    }
    if (!out || error) {
        std::filesystem::remove(temp, error);
        std::fprintf(stderr, "warning: cannot write %s; the day hashes stay in memory\n", path.c_str());
    }
}

std::vector<DayHandle> handles(const std::map<std::chrono::year_month_day, DayHandle>& days)
{
    std::vector<DayHandle> list;
    for (const auto& [date, day] : days) {
        list.push_back(day);
    }
    return list;
}

}  // namespace

std::uint64_t quick_check(std::span<const std::byte> bytes) noexcept
{
    const std::size_t edge = std::min(std::size_t{1} << 20, bytes.size());
    std::uint64_t hash = fnv1a_offset_basis;
    for (const std::span<const std::byte> part : {bytes.first(edge), bytes.last(edge)}) {
        for (const std::byte byte : part) {
            hash ^= std::to_integer<std::uint64_t>(byte);
            hash *= 1099511628211ULL;  // FNV-1a 64 prime
        }
    }
    return hash;
}

std::string day_hash(std::span<const std::byte> bytes)
{
    return hex(XXH3_128bits(bytes.data(), bytes.size()));
}

const Segment* Day::segment(std::string_view symbol) const
{
    const auto found = segments.find(symbol);
    return found == segments.end() ? nullptr : &found->second;
}

std::string segment_hash_as_read(const Day& day, std::string_view symbol)
{
    const Segment* segment = day.segment(symbol);
    std::vector<std::byte> bytes;
    std::string error;
    return segment != nullptr && read_segment(day.store, symbol, day.date, *segment, bytes, error) ? segment->hash
                                                                                                   : std::string{};
}

StoreLock::StoreLock(const std::filesystem::path& store)
{
    std::error_code error;
    std::filesystem::create_directories(store, error);
    fd_ = error ? -1 : ::open((store / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        error_ = error ? error.message() : std::strerror(errno);
    } else if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        error_ = errno == EWOULDBLOCK ? "in use by another chronos-serve" : std::strerror(errno);
    }
}

StoreLock::~StoreLock()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

std::string inputs_hash(std::span<const std::string> hashes)
{
    XXH3_state_t state;
    XXH3_128bits_reset(&state);
    for (const std::string& hash : hashes) {
        XXH3_128bits_update(&state, hash.data(), hash.size());
    }
    return hex(XXH3_128bits_digest(&state));
}

SourceFactory chain_factory(const DayLibrary& library, const Instrument& instrument, bool allow_truncated_tail,
                            bool watch)
{
    return [&library, instrument, allow_truncated_tail,
            watch](const Ticker& ticker, const DayList& days) -> std::unique_ptr<MarketEventSource> {
        std::vector<DayHandle> handles;
        for (std::size_t i = 0; i < days.count; ++i) {
            handles.push_back(library.find(days.day[i]));
        }
        if (name(ticker).empty() || std::find(handles.begin(), handles.end(), nullptr) != handles.end()) {
            return std::make_unique<VectorSource>(std::span<const MarketEvent>{});
        }
        return std::make_unique<DaySequenceSource>(std::move(handles), name(ticker), instrument, allow_truncated_tail,
                                                   watch);
    };
}

DayLibrary::DayLibrary(std::filesystem::path dir, std::filesystem::path cache, std::filesystem::path store)
    : dir_(std::move(dir)), cache_(std::move(cache)), store_(store.empty() ? dir_ / "store" : std::move(store))
{
    std::error_code error;
    std::filesystem::create_directories(store_, error);
    std::string why;
    if (index_.load(store_, why)) {
        return;
    }
    std::fprintf(stderr,
                 "the store index at %s is invalid (%s): it is moved to index.tsv.bad, and every day is built again\n",
                 store_.c_str(), why.c_str());
    std::filesystem::rename(store_ / "index.tsv", store_ / "index.tsv.bad", error);
    for (std::filesystem::directory_iterator file{store_, error};
         !error && file != std::filesystem::directory_iterator{}; file.increment(error)) {
        if (file->path().string().ends_with(".itch.zst")) {
            std::error_code ignored;
            std::filesystem::remove(file->path(), ignored);
        }
    }
}

// The store holds a day of `day`'s hash.
bool DayLibrary::current(const Day& day) const
{
    const auto stored = index_.days().find(day.date);
    return stored != index_.days().end() && stored->second.hash == day.hash;
}

// Reads a new version, and builds it into the store unless the store already holds its hash: a version of the size the
// store holds for its date is first read without building. False, said on stderr, if there is not room for the build,
// a pass fails or the build does not commit.
bool DayLibrary::build(const std::filesystem::path& path, Day& day)
{
    if (const auto held = index_.days().find(day.date); held != index_.days().end() && held->second.size == day.size) {
        std::fprintf(stderr, "checking %s, %" PRIu64 " bytes\n", day.file.c_str(), day.size);
        if (!identify(path, day, nullptr)) {
            return false;
        }
        if (current(day)) {
            return true;
        }
    }
    // A day's store takes about a third of its file: the check is an estimate, the build's abort the guarantee.
    std::error_code error;
    const std::uintmax_t free = std::filesystem::space(store_, error).available;
    if (!error && free < day.size / 5 * 3) {
        std::fprintf(stderr, "skipping %s: the store needs %" PRIu64 " MiB free, has %" PRIu64 " MiB\n",
                     day.file.c_str(), day.size / 5 * 3 >> 20, static_cast<std::uint64_t>(free >> 20));
        return false;
    }
    std::fprintf(stderr, "reading %s into the store, %" PRIu64 " bytes\n", day.file.c_str(), day.size);
    StoreBuilder builder{store_, path, day.date};
    if (!identify(path, day, &builder)) {
        return false;
    }
    std::string why;
    if (!builder.commit(day.hash, why)) {
        std::fprintf(stderr, "skipping %s: store build failed: %s\n", day.file.c_str(), why.c_str());
        return false;
    }
    index_ = builder.index();
    return true;
}

StoreUsage DayLibrary::usage()
{
    const std::lock_guard scanning{scan_mutex_};
    return store_usage(store_, index_);
}

DayHandle DayLibrary::find(std::chrono::year_month_day date) const
{
    const std::lock_guard lock{mutex_};
    const auto day = days_.find(date);
    return day == days_.end() ? nullptr : day->second;
}

std::vector<DayHandle> DayLibrary::scan()
{
    const std::lock_guard scanning{scan_mutex_};
    const nlohmann::json cache = read_cache(cache_);
    std::map<std::chrono::year_month_day, DayHandle> found;
    bool changed = false;
    std::error_code error;
    std::filesystem::directory_iterator files{dir_, error};
    for (; !error && files != std::filesystem::directory_iterator{}; files.increment(error)) {
        const auto day = std::make_shared<Day>();
        if (!open_version(files->path(), *day)) {
            continue;
        }
        if (const DayHandle known = find(day->date); known != nullptr && same_version(*known, *day)) {
            found.emplace(day->date, known);
            continue;
        }
        day->midnight = session_midnight_ns(day->date, us_eastern_utc_offset(day->date));
        // ponytail: a day whose build failed is read again each scan; retry only once its version changes, if needed.
        if ((!from_cache(cache, *day) || !current(*day)) && !build(files->path(), *day)) {
            continue;
        }
        day->store = store_;
        day->segments = index_.days().at(day->date).segments;
        for (const auto& [symbol, segment] : day->segments) {
            if (segment.events != 0) {
                day->symbols.push_back(symbol);
            }
        }
        found.emplace(day->date, day);
        changed = true;
    }
    if (error) {
        std::fprintf(stderr, "warning: cannot list %s: %s; the days stay as they were\n", dir_.c_str(),
                     error.message().c_str());
        const std::lock_guard lock{mutex_};
        return handles(days_);
    }
    std::map<std::chrono::year_month_day, DayHandle> retired;  // released after the lock: a last handle frees its Day
    {
        const std::lock_guard lock{mutex_};
        changed = changed || found.size() != days_.size();  // a file went
        retired = std::exchange(days_, found);
    }
    // ponytail: written as a scan ends, so a killed start reads the days it built again; write per build if needed.
    if (changed) {
        write_cache(cache_, found);
    }
    return handles(found);
}

DaySequenceSource::DaySequenceSource(std::vector<DayHandle> days, std::string_view ticker, const Instrument& instrument,
                                     bool allow_truncated_tail, bool hash_as_read)
    : days_(std::move(days)),
      ticker_(ticker),
      instrument_(instrument),
      allow_truncated_tail_(allow_truncated_tail),
      hash_as_read_(hash_as_read)
{
    if (std::any_of(days_.begin(), days_.end(), [](const DayHandle& day) { return !day->itch50; })) {
        error_ = SourceError::InvalidHeader;
    }
}

SourceResult DaySequenceSource::next(MarketEvent& event) noexcept
{
    while (error_ == SourceError::None && day_ < days_.size()) {
        const Day& day = *days_[day_];
        if (source_ == nullptr) {
            const Segment* segment = day.segment(ticker_);
            std::vector<std::byte> bytes;
            std::string why;
            if (segment != nullptr && !read_segment(day.store, ticker_, day.date, *segment, bytes, why)) {
                std::fprintf(stderr, "%s\n", why.c_str());
                error_ = SourceError::StoreRead;
            }
            if (hash_as_read_) {
                hashes_.push_back(segment != nullptr && error_ == SourceError::None ? segment->hash : "");
            }
            if (error_ != SourceError::None) {
                break;
            }
            // Without a segment, an empty day: its ItchSource ends in SymbolNotFound.
            auto source =
                std::make_unique<StoreSource>(std::move(bytes),
                                              ItchSourceConfig{.ticker = ticker_,
                                                               .session_date = day.date,
                                                               .utc_offset = us_eastern_utc_offset(day.date),
                                                               .allow_truncated_tail = allow_truncated_tail_});
            if (source->instrument().price_scale != instrument_.price_scale ||
                source->instrument().quantity_scale != instrument_.quantity_scale) {
                error_ = SourceError::InstrumentRejected;
                break;
            }
            source_ = std::move(source);
        }
        const SourceResult result = source_->next(event);
        if (result == SourceResult::Event) {
            return result;
        }
        if (result == SourceResult::Error) {
            error_ = source_->error();
            break;
        }
        const Segment* segment = day.segment(ticker_);
        before_ += segment != nullptr ? segment->uncompressed : 0;
        source_.reset();
        if (++day_ < days_.size()) {
            event = MarketEvent{.timestamp = days_[day_]->midnight, .type = MarketEventType::Clear};
            return SourceResult::Event;
        }
    }
    return error_ == SourceError::None ? SourceResult::EndOfStream : SourceResult::Error;
}

std::uint64_t DaySequenceSource::position() const noexcept
{
    return before_ + (source_ != nullptr ? source_->position() : 0);
}

SourceError DaySequenceSource::error() const noexcept
{
    return error_;
}

std::uint64_t DaySequenceSource::size() const noexcept
{
    std::uint64_t total = 0;
    for (const DayHandle& day : days_) {
        const Segment* segment = day->segment(ticker_);
        total += segment != nullptr ? segment->uncompressed : 0;
    }
    return total;
}

const std::vector<DayHandle>& DaySequenceSource::days() const noexcept
{
    return days_;
}

const std::vector<std::string>& DaySequenceSource::hashes() const noexcept
{
    return hashes_;
}

}  // namespace simulator::server
