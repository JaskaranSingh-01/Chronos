#include "simulator/store/store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <system_error>
#include <utility>

#define ZSTD_STATIC_LINKING_ONLY  // ZSTD_findDecompressedSize: zstd is pinned, and linked statically
#include <zstd.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "simulator/itch/itch_decoder.hpp"

namespace simulator {
namespace {

constexpr int compression_level = 3;

// As xxhsum prints it: the high half first.
std::string hex(XXH128_hash_t hash)
{
    char text[33];
    std::snprintf(text, sizeof text, "%016" PRIx64 "%016" PRIx64, hash.high64, hash.low64);
    return text;
}

std::string iso(std::chrono::year_month_day date)
{
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", int{date.year()}, unsigned{date.month()}, unsigned{date.day()});
    return text;
}

// A market event's type: an order message or a trade.
bool market_event(std::uint8_t type) noexcept
{
    return std::string_view{"AFECXDUP"}.find(static_cast<char>(type)) != std::string_view::npos;
}

// Flushes `fd` to the device: F_FULLFSYNC where it exists, as a macOS fsync leaves data in the drive's cache, and it
// flushes everything the drive holds, the files synced before it included.
bool sync_to_device(int fd) noexcept
{
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return true;
    }
    if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY) {
        return false;
    }
#endif
    return ::fsync(fd) == 0;
}

// fsync, or with `device` sync_to_device, of the file or directory at `path`.
bool sync_path(const std::filesystem::path& path, bool device = false) noexcept
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool synced = device ? sync_to_device(fd) : ::fsync(fd) == 0;
    const int error = errno;
    ::close(fd);
    errno = error;
    return synced;
}

// Writes all of `bytes` at `offset`; false with errno on an error, or EIO if nothing could be written.
bool write_at(int fd, std::span<const std::byte> bytes, std::uint64_t offset) noexcept
{
    while (!bytes.empty()) {
        const ssize_t written = ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (written <= 0) {
            errno = written == 0 ? EIO : errno;
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(written));
        offset += static_cast<std::uint64_t>(written);
    }
    return true;
}

// Reads all of `bytes` from `offset`; false with errno on an error, or EIO at the end of the file.
bool read_at(int fd, std::span<std::byte> bytes, std::uint64_t offset) noexcept
{
    while (!bytes.empty()) {
        const ssize_t got = ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (got <= 0) {
            errno = got == 0 ? EIO : errno;
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(got));
        offset += static_cast<std::uint64_t>(got);
    }
    return true;
}

// The file's size, inode and mtime, which a build checks did not change while it read the file.
struct Version
{
    std::uint64_t size{0};
    std::uint64_t inode{0};
    std::filesystem::file_time_type mtime{};

    bool operator==(const Version&) const = default;
};

bool version(const std::filesystem::path& path, Version& v) noexcept
{
    struct stat status{};
    std::error_code error;
    v.mtime = std::filesystem::last_write_time(path, error);
    if (error || ::stat(path.c_str(), &status) != 0) {
        return false;
    }
    v.size = static_cast<std::uint64_t>(status.st_size);
    v.inode = static_cast<std::uint64_t>(status.st_ino);
    return true;
}

}  // namespace

struct StoreBuilder::Build
{
    struct Buffer
    {
        std::filesystem::path file;
        std::vector<std::byte> pending;  // its frames not written yet
        XXH3_state_t hash;
        Segment segment;
        bool opened{false};  // its file was opened and cut to its committed end by this build
    };

    std::filesystem::path store;
    std::filesystem::path source;
    std::filesystem::path temp;  // the new index before its rename
    std::chrono::year_month_day date;
    StoreLimits limits;
    StoreIndex index;
    Version read{};      // the day file's when the build began
    std::string error;   // the first failure: nothing is written after it
    std::map<std::string, Buffer, std::less<>> buffers;      // by symbol
    std::vector<std::vector<Buffer*>> routes{std::size_t{1} << 16};  // by stock locate: the symbols its R named
    std::vector<std::byte> everyone;  // the frames every segment holds, so far
    std::uint64_t everyone_frames{0};
    std::size_t buffered{0};
    std::size_t peak{0};
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context{ZSTD_createCCtx(), ZSTD_freeCCtx};
    std::vector<std::byte> out;
    bool committed{false};

    // Errno's text as the build's error, unless one came first.
    void failed()
    {
        if (error.empty()) {
            error = std::strerror(errno);
        }
    }

    Buffer& begin(const std::string& symbol)
    {
        const auto [found, added] = buffers.try_emplace(symbol);
        Buffer& buffer = found->second;
        if (added) {
            buffer.file = store / symbol_file(symbol);
            XXH3_128bits_reset(&buffer.hash);
            buffer.segment.offset = index.committed_end(symbol);
            make_room(everyone.size());
            buffer.pending = everyone;
            buffer.segment.messages = everyone_frames;
            buffered += buffer.pending.capacity();
            peak = std::max(peak, buffered);
        }
        return buffer;
    }

    void append(Buffer& buffer, std::span<const std::byte> frame, bool event)
    {
        if (buffer.pending.size() + frame.size() > buffer.pending.capacity()) {
            // It doubles up to the flush size; `buffered` counts each buffer's capacity, what it holds allocated.
            make_room(std::max(frame.size(), std::min(buffer.pending.capacity(), limits.flush)));
            const std::size_t was = buffer.pending.capacity();
            buffer.pending.reserve(std::max(buffer.pending.size() + frame.size(), std::min(2 * was, limits.flush)));
            buffered += buffer.pending.capacity() - was;
            peak = std::max(peak, buffered);
        }
        buffer.pending.insert(buffer.pending.end(), frame.begin(), frame.end());
        ++buffer.segment.messages;
        buffer.segment.events += event ? 1 : 0;
        if (buffer.pending.size() >= limits.flush) {
            flush(buffer);
        }
    }

    void to_everyone(std::span<const std::byte> frame)
    {
        everyone.insert(everyone.end(), frame.begin(), frame.end());
        ++everyone_frames;
        for (auto& [symbol, buffer] : buffers) {
            append(buffer, frame, false);
        }
    }

    // Writes the largest buffers first, if `bytes` more would pass the cap, until under the low mark.
    void make_room(std::size_t bytes)
    {
        if (buffered + bytes <= limits.cap) {
            return;
        }
        std::vector<Buffer*> largest;
        for (auto& [symbol, buffer] : buffers) {
            largest.push_back(&buffer);
        }
        std::sort(largest.begin(), largest.end(),
                  [](const Buffer* a, const Buffer* b) { return a->pending.capacity() > b->pending.capacity(); });
        for (Buffer* buffer : largest) {
            if (buffered < limits.low) {
                return;
            }
            flush(*buffer);
        }
    }

    // Appends the buffer to its symbol's file as one zstd frame, first cutting the file to its committed end.
    void flush(Buffer& buffer)
    {
        if (buffer.pending.empty() || !error.empty()) {
            return;
        }
        XXH3_128bits_update(&buffer.hash, buffer.pending.data(), buffer.pending.size());
        out.resize(ZSTD_compressBound(buffer.pending.size()));
        const std::size_t size =
            ZSTD_compress2(context.get(), out.data(), out.size(), buffer.pending.data(), buffer.pending.size());
        if (ZSTD_isError(size)) {
            error = ZSTD_getErrorName(size);
            return;
        }
        const int fd = ::open(buffer.file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            failed();
            return;
        }
        const bool cut =
            std::exchange(buffer.opened, true) || ::ftruncate(fd, static_cast<off_t>(buffer.segment.offset)) == 0;
        const bool written =
            cut && write_at(fd, std::span{out}.first(size), buffer.segment.offset + buffer.segment.compressed);
        if (!written) {
            failed();
        }
        ::close(fd);
        buffer.segment.compressed += size;
        buffer.segment.uncompressed += buffer.pending.size();
        buffered -= buffer.pending.capacity();
        buffer.pending = std::vector<std::byte>{};  // frees it: an assignment of {} would keep the capacity
    }

    // Cuts each file this build wrote back to its committed end, and removes a new index not yet renamed.
    void abort() noexcept
    {
        for (const auto& [symbol, buffer] : buffers) {
            if (buffer.opened) {
                const int fd = ::open(buffer.file.c_str(), O_WRONLY | O_CLOEXEC);
                if (fd >= 0) {
                    (void)::ftruncate(fd, static_cast<off_t>(buffer.segment.offset));
                    ::close(fd);
                }
            }
        }
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
    }
};

StoreBuilder::StoreBuilder(std::filesystem::path store, std::filesystem::path source, std::chrono::year_month_day date,
                           StoreLimits limits)
    : build_(std::make_unique<Build>())
{
    Build& b = *build_;
    b.store = std::move(store);
    b.source = std::move(source);
    b.temp = b.store / "index.tsv.tmp";
    b.date = date;
    b.limits = limits;
    std::error_code created;
    std::filesystem::create_directories(b.store, created);
    std::string why;
    if (created) {
        b.error = created.message();
    } else if (!b.index.load(b.store, why)) {
        b.error = "the store index at " + b.store.string() + " is invalid: " + why;
    } else if (!version(b.source, b.read)) {
        b.failed();
    } else if (b.context == nullptr ||
               ZSTD_isError(ZSTD_CCtx_setParameter(b.context.get(), ZSTD_c_compressionLevel, compression_level)) ||
               ZSTD_isError(ZSTD_CCtx_setParameter(b.context.get(), ZSTD_c_checksumFlag, 1))) {
        b.error = "cannot start zstd";
    }
}

StoreBuilder::~StoreBuilder()
{
    if (!build_->committed) {
        build_->abort();
    }
}

void StoreBuilder::add(std::span<const std::byte> frame)
{
    Build& b = *build_;
    if (!b.error.empty()) {
        return;
    }
    const bool whole = frame.size() >= 3 &&
        (std::to_integer<std::size_t>(frame[0]) << 8 | std::to_integer<std::size_t>(frame[1])) == frame.size() - 2;
    const ItchMessage message{.type = whole ? std::to_integer<std::uint8_t>(frame[2]) : std::uint8_t{0},
                              .payload = whole ? frame.subspan(3) : std::span<const std::byte>{}};
    if (!whole || message.type == 'S') {
        b.to_everyone(frame);
        return;
    }
    if (message.type == 'R') {
        ItchDecoder decoder;
        ItchStockDirectory directory;
        if (decoder.decode_stock_directory(message, directory) != ItchDecodeResult::Success) {
            b.to_everyone(frame);  // it fails every symbol's replay of the day
            return;
        }
        const std::string_view stock{directory.stock.data(), directory.stock.size()};
        const std::string_view symbol = stock.substr(0, stock.find_last_not_of(' ') + 1);
        if (symbol.empty()) {
            return;  // ItchSource selects no ticker for it, and the index has no empty symbol
        }
        Build::Buffer& buffer = b.begin(std::string{symbol});
        std::vector<Build::Buffer*>& route = b.routes[directory.stock_locate];
        if (std::find(route.begin(), route.end(), &buffer) == route.end()) {
            route.push_back(&buffer);
        }
        b.append(buffer, frame, false);
        return;
    }
    if (message.payload.size() < 2) {
        if (market_event(message.type) || message.type == 'H') {
            b.to_everyone(frame);  // an event message ItchSource cannot route fails every symbol's replay
        }
        return;
    }
    const std::size_t locate =
        std::to_integer<std::size_t>(message.payload[0]) << 8 | std::to_integer<std::size_t>(message.payload[1]);
    for (Build::Buffer* buffer : b.routes[locate]) {
        b.append(*buffer, frame, market_event(message.type));
    }
}

bool StoreBuilder::commit(std::string_view hash, std::string& error)
{
    Build& b = *build_;
    if (b.error.empty() && b.source.filename().string() != day_file(b.date)) {
        b.error = "the day file is not named " + day_file(b.date);  // the index would not load
    }
    if (b.error.empty() && !is_hash(hash)) {
        b.error = "the day's hash is not 32 lower-case hex digits";
    }
    for (auto& [symbol, buffer] : b.buffers) {
        b.flush(buffer);
    }
    for (const auto& [symbol, buffer] : b.buffers) {
        if (b.error.empty() && buffer.opened && !sync_path(buffer.file)) {
            b.failed();
        }
    }
    if (b.error.empty() && !sync_path(b.store)) {
        b.failed();
    }
    Version now;
    if (b.error.empty() && (!version(b.source, now) || now != b.read)) {
        b.error = "the day file changed while it was read";
    }
    if (b.error.empty()) {
        StoreDay day{.file = b.source.filename().string(), .size = b.read.size, .hash = std::string{hash}};
        for (auto& [symbol, buffer] : b.buffers) {
            buffer.segment.hash = hex(XXH3_128bits_digest(&buffer.hash));
            day.segments.emplace(symbol, buffer.segment);
        }
        StoreIndex next = b.index;
        next.set(b.date, std::move(day));
        const std::string text = next.text();
        const int fd = ::open(b.temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        const bool written = fd >= 0 && write_at(fd, std::as_bytes(std::span{text}), 0) && sync_to_device(fd);
        if (!written) {
            b.failed();
        }
        if (fd >= 0) {
            ::close(fd);
        }
        if (written && ::rename(b.temp.c_str(), (b.store / "index.tsv").c_str()) == 0) {
            b.index = std::move(next);
            b.committed = true;
            // Best effort: the rename has committed, and a lost one reverts to the old index.
            (void)sync_path(b.store, true);
            return true;
        }
        if (written) {
            b.failed();
        }
    }
    error = b.error;
    b.abort();
    return false;
}

const StoreIndex& StoreBuilder::index() const noexcept
{
    return build_->index;
}

std::size_t StoreBuilder::peak_buffered() const noexcept
{
    return build_->peak;
}

bool read_segment(const std::filesystem::path& store, std::string_view symbol, std::chrono::year_month_day date,
                  const Segment& segment, std::vector<std::byte>& bytes, std::string& error)
{
    bytes.clear();
    const auto fail = [&](const std::string& why) {
        error = std::string{symbol} + " on " + iso(date) + ": " + why;
        bytes.clear();
        return false;
    };
    const int fd = ::open((store / symbol_file(symbol)).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return fail(std::strerror(errno));
    }
    std::vector<std::byte> compressed;
    struct stat status{};
    std::string why;
    try {
        if (::fstat(fd, &status) != 0) {
            why = std::strerror(errno);
        } else if (const auto size = static_cast<std::uint64_t>(status.st_size);
                   segment.compressed > size || segment.offset > size - segment.compressed) {
            why = "outside its file";
        } else {
            compressed.resize(segment.compressed);
            if (!read_at(fd, compressed, segment.offset)) {
                why = std::strerror(errno);
            }
        }
    } catch (const std::exception&) {  // a range no allocation can hold
        why = "cannot allocate " + std::to_string(segment.compressed) + " bytes";
    }
    ::close(fd);
    if (!why.empty()) {
        return fail(why);
    }
    // The frames' own lengths are checked against the index's before anything is sized by it.
    const unsigned long long framed = ZSTD_findDecompressedSize(compressed.data(), compressed.size());
    if (framed == ZSTD_CONTENTSIZE_ERROR) {
        return fail("damaged zstd frames");
    }
    if (framed == ZSTD_CONTENTSIZE_UNKNOWN) {
        return fail("zstd frames of no stated length");
    }
    if (framed != segment.uncompressed) {
        return fail("not its length");
    }
    try {
        bytes.resize(segment.uncompressed);
    } catch (const std::exception&) {  // a length no allocation can hold
        return fail("cannot allocate " + std::to_string(segment.uncompressed) + " bytes");
    }
    const std::size_t size = ZSTD_decompress(bytes.data(), bytes.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(size)) {
        return fail(ZSTD_getErrorName(size));
    }
    if (hex(XXH3_128bits(bytes.data(), bytes.size())) != segment.hash) {
        return fail("not its hash");
    }
    return true;
}

StoreUsage store_usage(const std::filesystem::path& store, const StoreIndex& index)
{
    StoreUsage usage;
    std::error_code error;
    for (std::filesystem::directory_iterator file{store, error};
         !error && file != std::filesystem::directory_iterator{}; file.increment(error)) {
        std::error_code unsized;
        const std::uintmax_t size = file->file_size(unsized);
        if (!unsized && file->path().string().ends_with(".itch.zst")) {
            usage.bytes += size;
        }
    }
    std::uint64_t held = 0;
    for (const auto& [date, day] : index.days()) {
        for (const auto& [symbol, segment] : day.segments) {
            held += segment.compressed;
        }
    }
    usage.unreferenced = usage.bytes - std::min(held, usage.bytes);
    return usage;
}

}  // namespace simulator
