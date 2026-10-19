#include "chronos/server/itch_input.hpp"

#include <sys/stat.h>

#include <algorithm>
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
#include "simulator/itch/byte_reader.hpp"
#include "simulator/itch/itch_decoder.hpp"
#include "simulator/itch/itch_message_reader.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_book.hpp"
#include "simulator/market/vector_source.hpp"

namespace simulator::server {
namespace {

// An ItchSource that owns the registry it registers its one instrument in.
class TickerSource final : public MarketEventSource
{
private:
    InstrumentRegistry registry_{1};
    ItchSource source_;

public:
    TickerSource(std::span<const std::byte> bytes, const ItchSourceConfig& config)
        : source_(bytes, config, registry_)
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

// The one pass over a new file version: its hash, its feed session and its ITCH 5.0 check. It reads the file a MiB at a
// time, at disk speed, where faulting the mapping in is several times slower. False, said on stderr, unless it read
// exactly the mapped size.
bool identify(const std::filesystem::path& path, Day& day)
{
    constexpr std::size_t step = std::size_t{1} << 20;
    constexpr std::size_t max_frame = 2 + 0xFFFF;  // the length prefix and the largest ITCH payload
    std::vector<std::byte> buffer(max_frame + step);  // the frame the last step cut, then a step
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
        while (framed && reader.next(message) == ItchReadResult::Message) {
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
        framed = framed && reader.error() != ItchReadError::InvalidLength;  // else a short frame: more to read
        carry = framed ? reader.remaining() : 0;
        std::memmove(buffer.data(), buffer.data() + reader.position(), carry);
    }
    if (in.bad() || !in.eof() || hashed != day.size) {
        std::fprintf(stderr, "skipping %s: read %" PRIu64 " of %" PRIu64 " bytes\n", day.file.c_str(), hashed,
                     day.size);
        return false;
    }
    day.cut_tail = framed && carry > 0;
    day.itch50 = day.itch50 && framed;
    day.hash = hex(XXH3_128bits_digest(&state));
    day.session = open && close && *open < *close
        ? FeedSession{.open = *open, .close = *close, .source = SessionSource::Feed}
        : FeedSession{.open = day.midnight + RTH_OPEN_NS, .close = day.midnight + RTH_CLOSE_NS};
    return true;
}

// Reads a day file's version: its date from its name, its mapping and size, its mtime and inode and its quick check.
// False if the name is not a day file's, or the file cannot be mapped or dated (said on stderr).
bool open_version(const std::filesystem::path& path, Day& day)
{
    day.file = path.filename().string();
    day.path = path;
    struct stat status{};
    if (!day_date(day.file, day.date) || ::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return false;
    }
    // ponytail: a file truncated in place can SIGBUS a reader of the mapping; copy or read() it if that matters.
    if (!day.mapped.open(path.c_str())) {
        std::fprintf(stderr, "skipping %s: %s\n", day.file.c_str(), day.mapped.error());
        return false;
    }
    std::error_code error;
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(path, error);
    if (error) {
        std::fprintf(stderr, "skipping %s: %s\n", day.file.c_str(), error.message().c_str());
        return false;
    }
    day.size = day.mapped.bytes().size();
    day.inode = static_cast<std::uint64_t>(status.st_ino);
    day.mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(written.time_since_epoch()).count();
    day.quick_check = quick_check(day.mapped.bytes());
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

std::vector<std::string> symbol_directory(std::span<const std::byte> bytes)
{
    std::vector<std::string> tickers;
    ItchMessageReader reader{bytes};
    ItchDecoder decoder;
    ItchMessage message;
    ItchStockDirectory directory;
    while (reader.next(message) == ItchReadResult::Message) {
        if (message.type == 'S' && message.payload.size() == 11 && message.payload[10] == std::byte{'S'}) {
            break;  // the start of system hours: the day's directory is complete
        }
        if (message.type == 'R' && decoder.decode_stock_directory(message, directory) == ItchDecodeResult::Success) {
            const std::string_view stock{directory.stock.data(), directory.stock.size()};
            tickers.emplace_back(stock.substr(0, stock.find_last_not_of(' ') + 1));
        }
    }
    std::sort(tickers.begin(), tickers.end());
    return tickers;
}

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

std::string file_hash(const std::filesystem::path& path)
{
    std::vector<std::byte> buffer(std::size_t{1} << 20);
    XXH3_state_t state;
    XXH3_128bits_reset(&state);
    std::ifstream in{path, std::ios::binary};
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        XXH3_128bits_update(&state, buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    return in.bad() || !in.eof() ? std::string{} : hex(XXH3_128bits_digest(&state));
}

std::string iso_date(std::chrono::year_month_day date)
{
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", int{date.year()}, unsigned{date.month()}, unsigned{date.day()});
    return text;
}

SourceFactory chain_factory(const DayLibrary& library, const Instrument& instrument, bool allow_truncated_tail)
{
    return [&library, instrument, allow_truncated_tail](const Ticker& ticker,
                                                        const DayList& days) -> std::unique_ptr<MarketEventSource> {
        std::vector<DayHandle> handles;
        for (std::size_t i = 0; i < days.count; ++i) {
            handles.push_back(library.find(days.day[i]));
        }
        if (name(ticker).empty() || std::find(handles.begin(), handles.end(), nullptr) != handles.end()) {
            return std::make_unique<VectorSource>(std::span<const MarketEvent>{});
        }
        return std::make_unique<DaySequenceSource>(std::move(handles), name(ticker), instrument, allow_truncated_tail,
                                                   false);
    };
}

DayLibrary::DayLibrary(std::filesystem::path dir, std::filesystem::path cache)
    : dir_(std::move(dir)), cache_(std::move(cache))
{}

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
        day->symbols = symbol_directory(day->mapped.bytes());
        if (!from_cache(cache, *day)) {
            std::fprintf(stderr, "hashing %s, %" PRIu64 " bytes\n", day->file.c_str(), day->size);
            if (!identify(files->path(), *day)) {
                continue;
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
    std::map<std::chrono::year_month_day, DayHandle> retired;  // released after the lock: a last handle unmaps
    {
        const std::lock_guard lock{mutex_};
        changed = changed || found.size() != days_.size();  // a file went
        retired = std::exchange(days_, found);
    }
    if (changed) {
        write_cache(cache_, found);
    }
    return handles(found);
}

struct DaySequenceSource::Hashing
{
    XXH3_state_t state;
    std::uint64_t hashed{0};  // the current day's bytes in the hash
};

DaySequenceSource::DaySequenceSource(std::vector<DayHandle> days, std::string_view ticker, const Instrument& instrument,
                                     bool allow_truncated_tail, bool hash_as_read)
    : days_(std::move(days)),
      ticker_(ticker),
      instrument_(instrument),
      allow_truncated_tail_(allow_truncated_tail),
      hashing_(hash_as_read ? std::make_unique<Hashing>() : nullptr)
{
    if (std::any_of(days_.begin(), days_.end(), [](const DayHandle& day) { return !day->itch50; })) {
        error_ = SourceError::InvalidHeader;
    }
}

DaySequenceSource::~DaySequenceSource() = default;

SourceResult DaySequenceSource::next(MarketEvent& event) noexcept
{
    constexpr std::uint64_t step = std::uint64_t{1} << 20;
    while (error_ == SourceError::None && day_ < days_.size()) {
        const Day& day = *days_[day_];
        const auto hash_to = [&](std::uint64_t end) {
            XXH3_128bits_update(&hashing_->state, day.mapped.bytes().data() + hashing_->hashed, end - hashing_->hashed);
            hashing_->hashed = end;
        };
        if (source_ == nullptr) {
            auto source =
                std::make_unique<TickerSource>(day.mapped.bytes(),
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
            if (hashing_ != nullptr) {
                XXH3_128bits_reset(&hashing_->state);
                hashing_->hashed = 0;
            }
        }
        const SourceResult result = source_->next(event);
        if (result == SourceResult::Event) {
            if (hashing_ != nullptr && source_->position() >= hashing_->hashed + step) {
                hash_to(source_->position() / step * step);  // whole MiBs already replayed
            }
            return result;
        }
        if (result == SourceResult::Error) {
            error_ = source_->error();
            break;
        }
        if (hashing_ != nullptr) {
            hash_to(day.size);
            hashes_.push_back(hex(XXH3_128bits_digest(&hashing_->state)));
        }
        before_ += day.size;
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
        total += day->size;
    }
    return total;
}

const std::vector<std::string>& DaySequenceSource::hashes() const noexcept
{
    return hashes_;
}

}  // namespace simulator::server
