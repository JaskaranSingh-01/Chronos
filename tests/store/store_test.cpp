#include <gtest/gtest.h>

#include <sys/resource.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "itch/itch_test_builder.hpp"
#include "simulator/cli/replay_args.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/itch/itch_message_reader.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_books.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/store/store.hpp"
#include "store_test_support.hpp"

namespace simulator::store_test {
namespace {

using namespace std::chrono_literals;
using itch_test::ItchBuilder;

constexpr std::chrono::year_month_day kMonday = 2019y / 12 / 30;
constexpr std::chrono::year_month_day kFriday = 2019y / 12 / 27;
const std::string kSourceHash = "0123456789abcdef0123456789abcdef";

std::filesystem::path day_path(const ScratchDir& dir, std::chrono::year_month_day date)
{
    return dir.path / (date == kMonday ? "12302019.NASDAQ_ITCH50.bin" : "12272019.NASDAQ_ITCH50.bin");
}

// The frames of `bytes` in order, a cut tail last.
std::vector<std::span<const std::byte>> frames(std::span<const std::byte> bytes)
{
    std::vector<std::span<const std::byte>> all;
    ItchMessageReader reader{bytes};
    ItchMessage message;
    std::size_t at = 0;
    while (reader.next(message) == ItchReadResult::Message) {
        all.push_back(bytes.subspan(at, reader.position() - at));
        at = reader.position();
    }
    if (at < bytes.size()) {
        all.push_back(bytes.subspan(at));
    }
    return all;
}

// Feeds `bytes` as chronos-serve's pass does: frame by frame, then a cut tail, or where the framing breaks, at a zero
// length, its two bytes alone.
void feed(StoreBuilder& builder, std::span<const std::byte> bytes)
{
    ItchMessageReader reader{bytes};
    ItchMessage message;
    std::size_t at = 0;
    while (reader.next(message) == ItchReadResult::Message) {
        builder.add(bytes.subspan(at, reader.position() - at));
        at = reader.position();
    }
    if (at < bytes.size()) {
        builder.add(bytes.subspan(at, reader.error() == ItchReadError::InvalidLength ? 2 : bytes.size() - at));
    }
}

// Writes `bytes` as the day file of `date` in `dir` and builds it into dir/store; "" once committed, else why not.
std::string build(const ScratchDir& dir, std::chrono::year_month_day date, std::span<const std::byte> bytes,
                  StoreLimits limits = {}, std::size_t* peak = nullptr)
{
    write_file(day_path(dir, date), {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    StoreBuilder builder{dir.path / "store", day_path(dir, date), date, limits};
    feed(builder, bytes);
    std::string error;
    builder.commit(kSourceHash, error);
    if (peak != nullptr) {
        *peak = builder.peak_buffered();
    }
    return error;
}

StoreIndex index_of(const ScratchDir& dir)
{
    StoreIndex index;
    std::string error;
    EXPECT_TRUE(index.load(dir.path / "store", error)) << error;
    return index;
}

// The segment of `symbol` on `date` as read back; empty, with a test failure, if it cannot be.
std::vector<std::byte> read_back(const ScratchDir& dir, std::chrono::year_month_day date, const std::string& symbol)
{
    const StoreIndex index = index_of(dir);
    std::vector<std::byte> bytes;
    std::string error;
    const auto day = index.days().find(date);
    EXPECT_NE(day, index.days().end());
    if (day != index.days().end() && day->second.segments.contains(symbol)) {
        EXPECT_TRUE(read_segment(dir.path / "store", symbol, date, day->second.segments.at(symbol), bytes, error))
            << error;
    }
    return bytes;
}

// The frames `keep` picks, concatenated.
template <class Keep>
std::vector<std::byte> picked(std::span<const std::byte> bytes, Keep&& keep)
{
    std::vector<std::byte> out;
    for (const std::span<const std::byte> frame : frames(bytes)) {
        if (keep(frame)) {
            out.insert(out.end(), frame.begin(), frame.end());
        }
    }
    return out;
}

char type(std::span<const std::byte> frame)
{
    return static_cast<char>(frame[2]);
}

std::uint16_t locate(std::span<const std::byte> frame)
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(frame[3]) << 8 | std::to_integer<unsigned>(frame[4]));
}

// AAA and BBB listed before the start of system hours, CCC after it, and a message of locate 9, which no R names.
ItchBuilder three_symbols()
{
    ItchBuilder itch;
    itch.system_event(0, 1'000, 'O')
        .stock_directory(1, 2'000, "AAA")
        .stock_directory(2, 2'000, "BBB")
        .trading_action(1, 2'500, "AAA", 'T')
        .system_event(0, 3'000, 'S')
        .system_event(0, 4'000, 'Q')
        .add_order(1, 5'000, 1, 'B', 100, "AAA", 100'000)
        .add_order(2, 5'100, 2, 'S', 200, "BBB", 200'000)
        .executed(1, 5'200, 1, 50, 1)
        .cancel(2, 5'300, 2, 100)
        .stock_directory(3, 6'000, "CCC")
        .add_order(3, 6'100, 3, 'B', 300, "CCC", 300'000)
        .erase(1, 6'200, 1)
        .add_order(9, 6'300, 4, 'B', 100, "ZZZ", 100'000)
        .system_event(0, 7'000, 'M');
    return itch;
}

// Each build is also run at a one-byte flush: every frame is written as it is added, so a System Event reaches buffers
// that are empty because they were just flushed.
const StoreLimits kFlushedAtOnce{.flush = 1};

TEST(StoreBuilderTest, ASegmentHoldsItsLocatesFramesFromItsDirectoryEntryAndEverySystemEventInFileOrder)
{
    for (const StoreLimits& limits : {StoreLimits{}, kFlushedAtOnce}) {
        SCOPED_TRACE(limits.flush);
        const ScratchDir dir{"store-routing-" + std::to_string(limits.flush)};
        const ItchBuilder day = three_symbols();
        ASSERT_EQ(build(dir, kMonday, day.bytes(), limits), "");

        const StoreIndex index = index_of(dir);
        ASSERT_EQ(index.days().size(), 1u);
        const StoreDay& monday = index.days().at(kMonday);
        EXPECT_EQ(monday.file, "12302019.NASDAQ_ITCH50.bin");
        EXPECT_EQ(monday.size, day.bytes().size());
        EXPECT_EQ(monday.hash, kSourceHash);
        ASSERT_EQ(monday.segments.size(), 3u);  // nothing for locate 9
        for (const auto& [symbol, at] : {std::pair{"AAA", 1}, std::pair{"BBB", 2}, std::pair{"CCC", 3}}) {
            bool listed = false;
            const std::vector<std::byte> expected = picked(day.bytes(), [&](std::span<const std::byte> frame) {
                listed = listed || (type(frame) == 'R' && locate(frame) == at);
                return type(frame) == 'S' || (listed && locate(frame) == at);
            });
            EXPECT_EQ(read_back(dir, kMonday, symbol), expected) << symbol;
            EXPECT_EQ(monday.segments.at(symbol).uncompressed, expected.size());
        }
        EXPECT_EQ(monday.segments.at("AAA").messages, 9u);  // 4 System Events, its R and H, A E D
        EXPECT_EQ(monday.segments.at("AAA").events, 3u);
        EXPECT_EQ(monday.segments.at("BBB").events, 2u);
        EXPECT_EQ(monday.segments.at("CCC").messages, 6u);  // listed late: the 3 System Events before it, then its own
        EXPECT_EQ(monday.segments.at("CCC").events, 1u);
        EXPECT_EQ(std::filesystem::file_size(dir.path / "store" / "AAA.itch.zst"),
                  monday.segments.at("AAA").compressed);
    }
}

// A frame ItchSource cannot route by locate fails every symbol's replay of the day, so it reaches every segment, as
// does a last frame cut short.
TEST(StoreBuilderTest, FramesNoLocateRoutesAndACutTailReachEverySegment)
{
    ItchBuilder itch = three_symbols();
    std::vector<std::byte> day(itch.bytes().begin(), itch.bytes().end());
    for (const char t : {'R', 'A', 'H', 'I'}) {  // an R too short to decode, then an A and an H too short to route
        day.insert(day.end(), {std::byte{0}, std::byte{2}, std::byte(t), std::byte{1}});
    }
    day.insert(day.end(), {std::byte{0}, std::byte{12}, std::byte{'S'}, std::byte{0}});  // a System Event, cut
    for (const StoreLimits& limits : {StoreLimits{}, kFlushedAtOnce}) {
        SCOPED_TRACE(limits.flush);
        const ScratchDir dir{"store-unroutable-" + std::to_string(limits.flush)};
        ASSERT_EQ(build(dir, kMonday, day, limits), "");
        for (const auto& [symbol, at] : {std::pair{"AAA", 1}, std::pair{"CCC", 3}}) {
            bool listed = false;
            const std::vector<std::byte> expected = picked(day, [&](std::span<const std::byte> frame) {
                if (frame.size() < 5) {
                    return type(frame) != 'I';  // an I nothing reads, too short to route, is left out
                }
                listed = listed || (type(frame) == 'R' && locate(frame) == at);
                return type(frame) == 'S' || (listed && locate(frame) == at);
            });
            EXPECT_EQ(read_back(dir, kMonday, symbol), expected) << symbol;
        }
    }
}

// An R naming no symbol, all spaces, selects no ticker in ItchSource: it and its locate's frames go nowhere, and the
// index, which has no empty symbol, still loads.
TEST(StoreBuilderTest, AnRWithoutASymbolRoutesNowhereAndTheDayStillCommitsAndLoads)
{
    const ScratchDir dir{"store-nameless"};
    ItchBuilder itch;
    itch.system_event(0, 1'000, 'O')
        .stock_directory(1, 2'000, "AAA")
        .stock_directory(2, 2'000, "        ")
        .add_order(1, 3'000, 1, 'B', 100, "AAA", 100'000)
        .add_order(2, 3'100, 2, 'B', 100, "AAA", 100'000)  // of the nameless locate
        .system_event(0, 4'000, 'M');
    ASSERT_EQ(build(dir, kMonday, itch.bytes()), "");

    const StoreIndex index = index_of(dir);
    ASSERT_EQ(index.days().at(kMonday).segments.size(), 1u);
    const std::vector<std::byte> expected =
        picked(itch.bytes(), [](std::span<const std::byte> frame) { return type(frame) == 'S' || locate(frame) == 1; });
    EXPECT_EQ(read_back(dir, kMonday, "AAA"), expected);
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator{dir.path / "store"}) {
        files.push_back(entry.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    EXPECT_EQ(files, (std::vector<std::string>{"AAA.itch.zst", "index.tsv"}));
}

// Many symbols, each flushed at 512 bytes, all of them at a 2 KiB cap: never more than the cap is buffered, and a
// segment of many zstd frames reads back whole.
TEST(StoreBuilderTest, NeverBuffersMoreThanTheCapAndASegmentOfManyFramesReadsBack)
{
    const ScratchDir dir{"store-cap"};
    ItchBuilder itch;
    itch.system_event(0, 1, 'O');
    for (std::uint16_t s = 1; s <= 12; ++s) {
        itch.stock_directory(s, 2, std::string(1, static_cast<char>('A' + s)));
    }
    for (std::uint64_t i = 0; i < 1'200; ++i) {
        const auto s = static_cast<std::uint16_t>(1 + i * 7 % 12);
        itch.add_order(s, 10 + i, i + 1, 'B', 100, std::string(1, static_cast<char>('A' + s)), 100'000);
    }
    std::size_t peak = 0;
    ASSERT_EQ(build(dir, kMonday, itch.bytes(), {.flush = 512, .cap = 2'048, .low = 1'024}, &peak), "");
    EXPECT_LE(peak, 2'048u);
    EXPECT_GT(peak, 1'024u);
    const std::vector<std::byte> b = read_back(dir, kMonday, "B");
    EXPECT_EQ(b.size(), 14u + 41 + 100 * 38);  // the System Event, its R and its hundred adds, each framed
    EXPECT_EQ(index_of(dir).days().at(kMonday).segments.at("B").events, 100u);
}

// The interrupted builds a crash leaves: appended tails and a new index not yet renamed. The committed store is in
// force; its tails count as unreferenced until the next build cuts them; then a complete commit reads back.
TEST(StoreBuilderTest, ACrashBeforeOrAfterTheRenameReopensToAValidStore)
{
    const ScratchDir dir{"store-crash"};
    const ItchBuilder friday = three_symbols();
    ASSERT_EQ(build(dir, kFriday, friday.bytes()), "");
    const std::filesystem::path store = dir.path / "store";
    const std::uint64_t committed = std::filesystem::file_size(store / "AAA.itch.zst");
    const std::vector<std::byte> before = read_back(dir, kFriday, "AAA");
    std::ofstream{store / "AAA.itch.zst", std::ios::app | std::ios::binary} << std::string(1'000, 't');  // a long tail
    write_file(store / "DDD.itch.zst", "a file it created");
    write_file(store / "index.tsv.tmp", "not an index");

    EXPECT_FALSE(before.empty());
    EXPECT_EQ(read_back(dir, kFriday, "AAA"), before);  // the tail is outside its segment
    EXPECT_EQ(store_usage(store, index_of(dir)).unreferenced, 1'000u + 17);

    ItchBuilder monday = three_symbols();
    monday.add_order(1, 8'000, 5, 'S', 100, "AAA", 100'100);
    ASSERT_EQ(build(dir, kMonday, monday.bytes()), "");
    EXPECT_FALSE(std::filesystem::exists(store / "index.tsv.tmp"));
    EXPECT_EQ(index_of(dir).days().at(kMonday).segments.at("AAA").offset, committed);  // the tail was cut first
    EXPECT_EQ(store_usage(store, index_of(dir)).unreferenced, 17u);  // DDD's file: no build has written DDD
    EXPECT_EQ(read_back(dir, kMonday, "AAA").size(), read_back(dir, kFriday, "AAA").size() + 38);
}

// Spec §6, recovery: under RLIMIT_FSIZE a write fails mid-build. Every file is back at its committed end, the old
// index is in force and its segments read back, and no new index is left behind.
TEST(StoreBuilderTest, AFailedWriteAbortsTheBuildAndLeavesTheCommittedStore)
{
    const ScratchDir dir{"store-fsize"};
    ASSERT_EQ(build(dir, kFriday, three_symbols().bytes()), "");
    const std::filesystem::path store = dir.path / "store";
    const std::string index_text = read_file(store / "index.tsv");
    const StoreIndex friday = index_of(dir);
    std::uint64_t largest = 0;
    for (const auto& [symbol, segment] : friday.days().at(kFriday).segments) {
        largest = std::max(largest, segment.offset + segment.compressed);
    }
    ItchBuilder monday = three_symbols();
    monday.stock_directory(4, 8'000, "AA0").add_order(4, 8'100, 9, 'B', 100, "AA0", 100'000);  // flushed before AAA
    write_file(day_path(dir, kMonday), {reinterpret_cast<const char*>(monday.bytes().data()), monday.bytes().size()});

    StoreBuilder builder{store, day_path(dir, kMonday), kMonday};
    feed(builder, monday.bytes());
    rlimit was{};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &was), 0);
    const auto ignored = std::signal(SIGXFSZ, SIG_IGN);  // so a write past the limit fails with EFBIG
    rlimit low = was;
    low.rlim_cur = largest + 16;
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &low), 0);
    std::string error;
    const bool committed = builder.commit(kSourceHash, error);
    ::setrlimit(RLIMIT_FSIZE, &was);
    std::signal(SIGXFSZ, ignored);

    EXPECT_FALSE(committed);
    EXPECT_EQ(error, std::strerror(EFBIG));
    EXPECT_EQ(read_file(store / "index.tsv"), index_text);
    EXPECT_FALSE(std::filesystem::exists(store / "index.tsv.tmp"));
    const StoreIndex index = index_of(dir);
    for (const auto& [symbol, segment] : index.days().at(kFriday).segments) {
        EXPECT_EQ(std::filesystem::file_size(store / symbol_file(symbol)), segment.offset + segment.compressed);
        EXPECT_FALSE(read_back(dir, kFriday, symbol).empty());
    }
    EXPECT_EQ(std::filesystem::file_size(store / "AA0.itch.zst"), 0u);
    EXPECT_EQ(store_usage(store, index).unreferenced, 0u);
}

// Spec §2.5: the day file replaced, or written in place, during a build: nothing is committed.
TEST(StoreBuilderTest, ADayFileChangedWhileItIsReadIsNotCommittedButOneChangedAfterACommitIsIgnored)
{
    const ScratchDir dir{"store-source"};
    const ItchBuilder day = three_symbols();
    for (const bool replace : {true, false}) {
        write_file(day_path(dir, kMonday), {reinterpret_cast<const char*>(day.bytes().data()), day.bytes().size()});
        StoreBuilder builder{dir.path / "store", day_path(dir, kMonday), kMonday};
        feed(builder, day.bytes());
        if (replace) {
            write_file(dir.path / "new", "another version");
            std::filesystem::rename(dir.path / "new", day_path(dir, kMonday));
        } else {
            std::ofstream{day_path(dir, kMonday), std::ios::app | std::ios::binary} << "more";
        }
        std::string error;
        EXPECT_FALSE(builder.commit(kSourceHash, error));
        EXPECT_EQ(error, "the day file changed while it was read");
        EXPECT_FALSE(std::filesystem::exists(dir.path / "store" / "index.tsv"));
        EXPECT_EQ(std::filesystem::file_size(dir.path / "store" / "AAA.itch.zst"), 0u);
    }

    write_file(day_path(dir, kMonday), {reinterpret_cast<const char*>(day.bytes().data()), day.bytes().size()});
    StoreBuilder builder{dir.path / "store", day_path(dir, kMonday), kMonday};
    feed(builder, day.bytes());
    std::string error;
    ASSERT_TRUE(builder.commit(kSourceHash, error)) << error;
    const std::string index = read_file(dir.path / "store" / "index.tsv");
    const auto size = std::filesystem::file_size(dir.path / "store" / "AAA.itch.zst");
    std::ofstream{day_path(dir, kMonday), std::ios::app | std::ios::binary} << "more";
    EXPECT_TRUE(builder.commit(kSourceHash, error));  // the first commit stands
    EXPECT_EQ(read_file(dir.path / "store" / "index.tsv"), index);
    EXPECT_EQ(std::filesystem::file_size(dir.path / "store" / "AAA.itch.zst"), size);
}

// A builder dropped uncommitted, after it flushed as it went, cuts every file back to its committed end.
TEST(StoreBuilderTest, ABuilderDroppedAfterFlushingLeavesEveryFileAtItsCommittedEnd)
{
    const ScratchDir dir{"store-dropped"};
    const std::filesystem::path store = dir.path / "store";
    ASSERT_EQ(build(dir, kFriday, three_symbols().bytes()), "");
    const std::string index_text = read_file(store / "index.tsv");
    ItchBuilder monday = three_symbols();
    monday.stock_directory(4, 8'000, "DDD").add_order(4, 8'100, 9, 'B', 100, "DDD", 100'000);  // a file of its own
    write_file(day_path(dir, kMonday), {reinterpret_cast<const char*>(monday.bytes().data()), monday.bytes().size()});
    {
        StoreBuilder builder{store, day_path(dir, kMonday), kMonday, kFlushedAtOnce};
        feed(builder, monday.bytes());
        EXPECT_GT(std::filesystem::file_size(store / "AAA.itch.zst"),
                  index_of(dir).days().at(kFriday).segments.at("AAA").compressed);  // written, not yet committed
        EXPECT_GT(std::filesystem::file_size(store / "DDD.itch.zst"), 0u);
    }
    const StoreIndex index = index_of(dir);
    for (const auto& [symbol, segment] : index.days().at(kFriday).segments) {
        EXPECT_EQ(std::filesystem::file_size(store / symbol_file(symbol)), segment.offset + segment.compressed);
    }
    EXPECT_EQ(std::filesystem::file_size(store / "DDD.itch.zst"), 0u);
    EXPECT_EQ(read_file(store / "index.tsv"), index_text);
    EXPECT_FALSE(std::filesystem::exists(store / "index.tsv.tmp"));
    EXPECT_EQ(store_usage(store, index).unreferenced, 0u);
}

// A commit never writes an index StoreIndex::load would refuse: a source not named for its day, or a hash that is not
// 32 lower-case hex digits, aborts the build.
TEST(StoreBuilderTest, ACommitOfASourceNotNamedForItsDayOrOfAMalformedHashIsRefused)
{
    const ScratchDir dir{"store-guard"};
    const std::filesystem::path store = dir.path / "store";
    const ItchBuilder day = three_symbols();
    const std::string text{reinterpret_cast<const char*>(day.bytes().data()), day.bytes().size()};
    write_file(day_path(dir, kMonday), text);
    write_file(dir.path / "monday.bin", text);
    const auto refused = [&](const std::filesystem::path& source, const std::string& hash) {
        StoreBuilder builder{store, source, kMonday, kFlushedAtOnce};
        feed(builder, day.bytes());
        std::string error;
        EXPECT_FALSE(builder.commit(hash, error));
        EXPECT_FALSE(std::filesystem::exists(store / "index.tsv"));
        EXPECT_EQ(std::filesystem::file_size(store / "AAA.itch.zst"), 0u);
        return error;
    };
    EXPECT_EQ(refused(dir.path / "monday.bin", kSourceHash), "the day file is not named 12302019.NASDAQ_ITCH50.bin");
    EXPECT_EQ(refused(day_path(dir, kMonday), "0123456789ABCDEF0123456789ABCDEF"),
              "the day's hash is not 32 lower-case hex digits");
    EXPECT_EQ(refused(day_path(dir, kMonday), kSourceHash.substr(1)), "the day's hash is not 32 lower-case hex digits");
    EXPECT_EQ(build(dir, kMonday, day.bytes()), "");  // and the store is as good as it was
    EXPECT_EQ(index_of(dir).days().at(kMonday).segments.size(), 3u);
}

// Spec §2.5, dead bytes: a rebuilt day's old segments stay in their files, unreferenced; another day's build, or a
// failed one, leaves the count as it was.
TEST(StoreBuilderTest, ARebuiltDaysOldSegmentsAreTheUnreferencedBytes)
{
    const ScratchDir dir{"store-dead"};
    const std::filesystem::path store = dir.path / "store";
    ASSERT_EQ(build(dir, kMonday, three_symbols().bytes()), "");
    EXPECT_EQ(store_usage(store, index_of(dir)).unreferenced, 0u);
    const StoreIndex first = index_of(dir);
    std::uint64_t old = 0;
    for (const auto& [symbol, segment] : first.days().at(kMonday).segments) {
        old += segment.compressed;
    }

    ItchBuilder changed = three_symbols();
    changed.add_order(2, 8'000, 5, 'B', 100, "BBB", 199'900);
    ASSERT_EQ(build(dir, kMonday, changed.bytes()), "");
    const StoreUsage rebuilt = store_usage(store, index_of(dir));
    EXPECT_EQ(rebuilt.unreferenced, old);
    EXPECT_EQ(read_back(dir, kMonday, "BBB").size(), 4u * 14 + 41 + 38 + 25 + 38);

    ASSERT_EQ(build(dir, kFriday, three_symbols().bytes()), "");
    EXPECT_EQ(store_usage(store, index_of(dir)).unreferenced, rebuilt.unreferenced);
    write_file(day_path(dir, kFriday), "replaced");
    StoreBuilder failed{store, day_path(dir, kFriday), kFriday};
    feed(failed, changed.bytes());
    std::filesystem::rename(day_path(dir, kFriday), dir.path / "moved");
    std::string error;
    EXPECT_FALSE(failed.commit(kSourceHash, error));
    EXPECT_EQ(store_usage(store, index_of(dir)).unreferenced, rebuilt.unreferenced);
}

// read_segment never trusts the index alone: a file damaged, cut or gone, or an index that misstates the length or
// the hash, is an error naming the symbol and day, never bytes.
TEST(StoreReadTest, ADamagedOrMisdescribedSegmentIsAnErrorNamingTheSymbolAndDay)
{
    const ScratchDir dir{"store-damage"};
    ASSERT_EQ(build(dir, kMonday, three_symbols().bytes()), "");
    const std::filesystem::path store = dir.path / "store";
    const Segment aaa = index_of(dir).days().at(kMonday).segments.at("AAA");
    const auto read = [&](Segment segment) {
        std::vector<std::byte> bytes;
        std::string error;
        EXPECT_FALSE(read_segment(store, "AAA", kMonday, segment, bytes, error));
        EXPECT_TRUE(bytes.empty());
        return error;
    };
    Segment longer = aaa;
    ++longer.uncompressed;
    EXPECT_EQ(read(longer), "AAA on 2019-12-30: not its length");
    Segment shorter = aaa;
    --shorter.uncompressed;
    EXPECT_EQ(read(shorter), "AAA on 2019-12-30: not its length");
    Segment other = aaa;
    other.hash = kSourceHash;
    EXPECT_EQ(read(other), "AAA on 2019-12-30: not its hash");
    Segment past = aaa;
    ++past.offset;
    EXPECT_EQ(read(past), "AAA on 2019-12-30: outside its file");

    std::string bytes = read_file(store / "AAA.itch.zst");
    bytes.back() ^= 0x01;  // in the zstd frame's content checksum
    write_file(store / "AAA.itch.zst", bytes);
    EXPECT_EQ(read(aaa), "AAA on 2019-12-30: Restored data doesn't match checksum");
    std::filesystem::remove(store / "AAA.itch.zst");
    EXPECT_EQ(read(aaa), "AAA on 2019-12-30: No such file or directory");
}

// The index's length sizes nothing until the zstd frames themselves state it: a length no frame has, however large, is
// "not its length", and frames that state none, or are not frames, are errors too.
TEST(StoreReadTest, AnIndexedLengthIsCheckedAgainstTheFramesBeforeAnythingIsSizedByIt)
{
    const ScratchDir dir{"store-length"};
    ASSERT_EQ(build(dir, kMonday, three_symbols().bytes()), "");
    const std::filesystem::path store = dir.path / "store";
    Segment aaa = index_of(dir).days().at(kMonday).segments.at("AAA");
    const auto read = [&](const Segment& segment) {
        std::vector<std::byte> bytes;
        std::string error;
        EXPECT_FALSE(read_segment(store, "AAA", kMonday, segment, bytes, error));
        EXPECT_TRUE(bytes.empty());
        return error;
    };
    Segment huge = aaa;
    huge.uncompressed = std::uint64_t{1} << 62;
    EXPECT_EQ(read(huge), "AAA on 2019-12-30: not its length");

    aaa.offset = 0;
    aaa.uncompressed = std::uint64_t{1} << 62;
    aaa.compressed = 9;
    write_file(store / "AAA.itch.zst",
               std::string{"\x28\xB5\x2F\xFD\x00\x00\x01\x00\x00", 9});  // an empty frame, size unstated
    EXPECT_EQ(read(aaa), "AAA on 2019-12-30: zstd frames of no stated length");
    write_file(store / "AAA.itch.zst", std::string(9, 'x'));
    EXPECT_EQ(read(aaa), "AAA on 2019-12-30: damaged zstd frames");
}

// A read racing chronos-serve's startup discard of an invalid index (spec §2.5) returns the intact segment or fails
// cleanly: after the files go it fails, and the bytes rebuilt in their place read back only if they are the same.
TEST(StoreReadTest, AReadOfAStoreRebuiltInPlaceIsTheIntactSegmentOrAnError)
{
    const ScratchDir dir{"store-rebuilt"};
    const ItchBuilder day = three_symbols();
    ASSERT_EQ(build(dir, kMonday, day.bytes()), "");
    const std::filesystem::path store = dir.path / "store";
    const Segment aaa = index_of(dir).days().at(kMonday).segments.at("AAA");
    const std::vector<std::byte> intact = read_back(dir, kMonday, "AAA");
    std::filesystem::remove_all(store);
    std::vector<std::byte> bytes;
    std::string error;
    EXPECT_FALSE(read_segment(store, "AAA", kMonday, aaa, bytes, error));

    ItchBuilder other = three_symbols();
    other.add_order(1, 6'500, 6, 'S', 100, "AAA", 100'200);  // AAA's bytes differ, in place of the old
    ASSERT_EQ(build(dir, kMonday, other.bytes()), "");
    EXPECT_FALSE(read_segment(store, "AAA", kMonday, aaa, bytes, error));
    EXPECT_TRUE(bytes.empty());

    std::filesystem::remove_all(store);
    ASSERT_EQ(build(dir, kMonday, day.bytes()), "");  // the same bytes rebuilt: the same segment
    ASSERT_TRUE(read_segment(store, "AAA", kMonday, aaa, bytes, error)) << error;
    EXPECT_EQ(bytes, intact);
}

// Spec §2.4: every event type over AAA and BBB, interleaved; a System Event before any symbol's message; BBB halted by
// the sweep at the start of system hours and again mid-day; CCC listed after it; a message no R names.
ItchBuilder busy_day(std::uint64_t start_of_system_hours = 3'000)
{
    ItchBuilder itch;
    itch.system_event(0, 1'000, 'O')
        .stock_directory(1, 2'000, "AAA")
        .stock_directory(2, 2'000, "BBB")
        .trading_action(1, 2'500, "AAA", 'T')
        .system_event(0, start_of_system_hours, 'S')
        .trading_action(2, 3'500, "BBB", 'T')
        .system_event(0, 4'000, 'Q')
        .add_order(1, 5'000, 1, 'B', 100, "AAA", 100'000)
        .add_order_mpid(2, 5'100, 2, 'S', 200, "BBB", 200'000, "MPID")
        .executed(1, 5'200, 1, 50, 1)
        .executed_with_price(2, 5'300, 2, 20, 2, true, 199'900)
        .stock_directory(3, 6'000, "CCC")
        .add_order(3, 6'100, 3, 'B', 300, "CCC", 300'000)
        .replace(1, 6'200, 1, 4, 50, 100'100)
        .trading_action(2, 6'300, "BBB", 'H')
        .cancel(2, 6'400, 2, 30)
        .trade(1, 6'500, 0, 'B', 10, "AAA", 100'050, 3)
        .trading_action(2, 6'600, "BBB", 'T')
        .erase(1, 6'700, 4)
        .erase(3, 6'800, 3)
        .add_order(9, 6'900, 5, 'B', 100, "ZZZ", 100'000)
        .system_event(0, 7'000, 'M');
    return itch;
}

using Event =
    std::tuple<Timestamp, OrderId, OrderId, std::int64_t, std::uint64_t, SymbolId, MarketEventType, Side, TradingState>;

// What an ItchSource of `ticker` decodes from `bytes`: each event, then how the stream ended.
struct Decoded
{
    std::vector<Event> events;
    SourceResult end{SourceResult::Event};
    SourceError error{SourceError::None};
    bool truncated_tail{false};

    bool operator==(const Decoded&) const = default;
};

Decoded decode(std::span<const std::byte> bytes, std::string_view ticker, bool allow_truncated_tail)
{
    InstrumentRegistry registry{1};
    ItchSource source{bytes,
                      {.ticker = ticker,
                       .session_date = kMonday,
                       .utc_offset = us_eastern_utc_offset(kMonday),
                       .allow_truncated_tail = allow_truncated_tail},
                      registry};
    Decoded decoded;
    MarketEvent e;
    while ((decoded.end = source.next(e)) == SourceResult::Event) {
        decoded.events.emplace_back(e.timestamp, e.order_id, e.new_order_id, e.price.ticks, e.quantity.value(),
                                    e.symbol, e.type, e.side, e.trading_state);
    }
    decoded.error = source.error();
    decoded.truncated_tail = source.truncated_tail();
    return decoded;
}

// Spec §2.4 and §6, event-stream equality: for every symbol, its segment decodes to exactly the events its day file
// does, ending the same way: a whole day, one cut in its last message, one with frames no locate routes, and one whose
// framing breaks mid-file.
TEST(StoreEquivalenceTest, EverySymbolsSegmentDecodesToTheEventsOfItsDayFile)
{
    const ItchBuilder busy = busy_day();
    const std::span<const std::byte> whole = busy.bytes();
    std::vector<std::byte> unroutable(whole.begin(), whole.end() - 14);  // before the end of system hours
    unroutable.insert(unroutable.end(), {std::byte{0}, std::byte{2}, std::byte{'R'}, std::byte{1}});
    unroutable.insert(unroutable.end(), whole.end() - 14, whole.end());
    std::vector<std::byte> broken(whole.begin(), whole.end());
    broken.insert(broken.begin() + (frames(whole)[13].data() - whole.data()),  // a zero length after CCC's first order
                  {std::byte{0}, std::byte{0}});
    const std::vector<std::pair<std::string, std::span<const std::byte>>> days{
        {"whole", whole}, {"cut", whole.first(whole.size() - 5)}, {"unroutable", unroutable}, {"broken", broken}};
    for (const auto& [name, bytes] : days) {
        const ScratchDir dir{"store-equal-" + name};
        ASSERT_EQ(build(dir, kMonday, bytes), "") << name;
        for (const std::string symbol : {"AAA", "BBB", "CCC"}) {
            const std::vector<std::byte> segment = read_back(dir, kMonday, symbol);
            for (const bool allow : {false, true}) {
                EXPECT_EQ(decode(segment, symbol, allow), decode(bytes, symbol, allow))
                    << name << " " << symbol << (allow ? " allowing a cut tail" : "");
            }
        }
    }
    EXPECT_EQ(decode(whole, "AAA", false).events.size(), 6u);  // H, A, E, U, P, D: the sweep skips AAA, which has an H
    EXPECT_EQ(decode(whole, "BBB", false).events.size(), 7u);  // halted by the sweep, then H T, F, C, H H, X, H T
    EXPECT_EQ(decode(whole.first(whole.size() - 5), "CCC", false).error, SourceError::TruncatedMessage);
    EXPECT_EQ(decode(unroutable, "CCC", true).error, SourceError::InvalidPayloadLength);
    EXPECT_EQ(decode(broken, "AAA", true).events.size(), 3u);  // H, A, E
    EXPECT_EQ(decode(broken, "CCC", true).error, SourceError::InvalidLength);
}

// Spec §6, System Events: moving the start of system hours changes every segment of the day, and each decodes to the
// day's new stream, the halt sweep at the new time.
TEST(StoreEquivalenceTest, AChangedSystemEventChangesEverySegmentAndItsStream)
{
    const ScratchDir before{"store-system-before"};
    const ScratchDir after{"store-system-after"};
    const ItchBuilder moved = busy_day(3'100);
    ASSERT_EQ(build(before, kMonday, busy_day().bytes()), "");
    ASSERT_EQ(build(after, kMonday, moved.bytes()), "");
    for (const std::string symbol : {"AAA", "BBB", "CCC"}) {
        EXPECT_NE(index_of(before).days().at(kMonday).segments.at(symbol).hash,
                  index_of(after).days().at(kMonday).segments.at(symbol).hash)
            << symbol;
        EXPECT_EQ(decode(read_back(after, kMonday, symbol), symbol, false), decode(moved.bytes(), symbol, false));
    }
    EXPECT_EQ(std::get<0>(decode(read_back(after, kMonday, "BBB"), "BBB", false).events[0]),
              session_midnight_ns(kMonday, us_eastern_utc_offset(kMonday)) + 3'100);
}

// Spec §6, symbol isolation: another symbol's message changed leaves this symbol's segment byte for byte.
TEST(StoreEquivalenceTest, AnotherSymbolsChangedMessageLeavesThisSegmentAsItWas)
{
    const ScratchDir before{"store-isolation-before"};
    const ScratchDir after{"store-isolation-after"};
    ItchBuilder changed = busy_day();
    changed.add_order(2, 7'100, 6, 'B', 100, "BBB", 199'000);
    ASSERT_EQ(build(before, kMonday, busy_day().bytes()), "");
    ASSERT_EQ(build(after, kMonday, changed.bytes()), "");
    const auto segment = [](const ScratchDir& dir, const std::string& symbol) {
        return index_of(dir).days().at(kMonday).segments.at(symbol);
    };
    EXPECT_EQ(segment(after, "AAA"), segment(before, "AAA"));
    EXPECT_EQ(read_back(after, kMonday, "AAA"), read_back(before, kMonday, "AAA"));
    EXPECT_NE(segment(after, "BBB").hash, segment(before, "BBB").hash);
}

// Spec §6, digest equality on a real day (CHRONOS_ITCH_FILE, CHRONOS_ITCH_DATE as in itch_real_data_test.cpp): the
// store is built in the temp directory, then every symbol's segment replays to the book that symbol has at the end of
// the full-day replay of every symbol. Needs about a third of the day file's size free in TMPDIR.
TEST(StoreRealDataTest, EverySymbolsSegmentReplaysToItsBookOfTheFullDay)
{
    const char* path = std::getenv("CHRONOS_ITCH_FILE");
    if (path == nullptr || *path == '\0') {
        GTEST_SKIP() << "set CHRONOS_ITCH_FILE to a NASDAQ ITCH 5.0 file";
    }
    const char* day = std::getenv("CHRONOS_ITCH_DATE");
    std::chrono::year_month_day date{};
    ASSERT_TRUE(parse_date(day != nullptr && *day != '\0' ? day : "2019-12-30", date));
    const Timestamp midnight = session_midnight_ns(date, us_eastern_utc_offset(date));
    const ReplayOptions options{.session_midnight = midnight};
    MappedFile file;
    ASSERT_TRUE(file.open(path)) << file.error();

    const ScratchDir dir{"store-real"};
    const std::filesystem::path source = dir.path / day_file(date);  // commit() takes only a day file's name
    std::filesystem::create_symlink(std::filesystem::absolute(path), source);
    StoreBuilder builder{dir.path, source, date};
    feed(builder, file.bytes());
    std::string error;
    ASSERT_TRUE(builder.commit(kSourceHash, error)) << error;

    InstrumentRegistry registry{std::size_t{1} << 16};
    ItchSource all{file.bytes(),
                   {.all_tickers = true,
                    .session_date = date,
                    .utc_offset = us_eastern_utc_offset(date),
                    .allow_truncated_tail = true},
                   registry};
    BookStorage storage{default_books_capacity};
    MarketBooks books{registry, storage};
    ASSERT_EQ(MarketReplay::run(all, books, options).total.failure, ReplayFailure::None);
    std::map<std::string, SymbolId, std::less<>> ids;
    for (SymbolId id = 0; id < registry.size(); ++id) {
        const auto& symbol = registry.find(id)->native_symbol;
        ids.emplace(
            std::string{symbol.data(),
                        static_cast<std::size_t>(std::find(symbol.begin(), symbol.end(), '\0') - symbol.begin())},
            id);
    }

    std::size_t compared = 0;
    std::size_t differ = 0;
    for (const auto& [symbol, segment] : builder.index().days().at(date).segments) {
        std::vector<std::byte> bytes;
        ASSERT_TRUE(read_segment(dir.path, symbol, date, segment, bytes, error)) << error;
        InstrumentRegistry one{1};
        ItchSource source{bytes,
                          {.ticker = symbol,
                           .session_date = date,
                           .utc_offset = us_eastern_utc_offset(date),
                           .allow_truncated_tail = true},
                          one};
        MarketBook book{0, std::size_t{1} << 20};
        MarketReplay::run(source, book, options);
        const MarketBook& full = *books.find(ids.at(symbol));
        ++compared;
        if (book.digest() != full.digest() || book.last_sequence() != full.last_sequence()) {
            ++differ;
            ADD_FAILURE() << symbol << ": digest " << book.digest() << ", the full day's " << full.digest();
        }
    }
    std::printf("[ RealData ] %zu symbols' segments, %zu differ from the full day\n", compared, differ);
    EXPECT_GT(compared, 0u);
    EXPECT_EQ(compared, registry.size());
}

}  // namespace
}  // namespace simulator::store_test
