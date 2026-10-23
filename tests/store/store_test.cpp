#include <gtest/gtest.h>

#include <sys/resource.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "itch/itch_test_builder.hpp"
#include "simulator/itch/itch_message_reader.hpp"
#include "simulator/store/store.hpp"
#include "store_test_support.hpp"

namespace simulator::store_test {
namespace {

using namespace std::chrono_literals;
using itch_test::ItchBuilder;

constexpr std::chrono::year_month_day kMonday = 2019y / 12 / 30;
constexpr std::chrono::year_month_day kFriday = 2019y / 12 / 27;
const std::string kSourceHash = "0123456789abcdef0123456789abcdef";

std::filesystem::path day_file(const ScratchDir& dir, std::chrono::year_month_day date)
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

void feed(StoreBuilder& builder, std::span<const std::byte> bytes)
{
    for (const std::span<const std::byte> frame : frames(bytes)) {
        builder.add(frame);
    }
}

// Writes `bytes` as the day file of `date` in `dir` and builds it into dir/store; "" once committed, else why not.
std::string build(const ScratchDir& dir, std::chrono::year_month_day date, std::span<const std::byte> bytes,
                  StoreLimits limits = {}, std::size_t* peak = nullptr)
{
    write_file(day_file(dir, date), {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    StoreBuilder builder{dir.path / "store", day_file(dir, date), date, limits};
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

TEST(StoreBuilderTest, ASegmentHoldsItsLocatesFramesFromItsDirectoryEntryAndEverySystemEventInFileOrder)
{
    const ScratchDir dir{"store-routing"};
    const ItchBuilder day = three_symbols();
    ASSERT_EQ(build(dir, kMonday, day.bytes()), "");

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
    EXPECT_EQ(std::filesystem::file_size(dir.path / "store" / "AAA.itch.zst"), monday.segments.at("AAA").compressed);
}

// A frame ItchSource cannot route by locate fails every symbol's replay of the day, so it reaches every segment, as
// does a last frame cut short.
TEST(StoreBuilderTest, FramesNoLocateRoutesAndACutTailReachEverySegment)
{
    const ScratchDir dir{"store-unroutable"};
    ItchBuilder itch = three_symbols();
    std::vector<std::byte> day(itch.bytes().begin(), itch.bytes().end());
    for (const char t : {'R', 'A', 'H', 'I'}) {  // an R too short to decode, then an A and an H too short to route
        day.insert(day.end(), {std::byte{0}, std::byte{2}, std::byte(t), std::byte{1}});
    }
    day.insert(day.end(), {std::byte{0}, std::byte{12}, std::byte{'S'}, std::byte{0}});  // a System Event, cut
    ASSERT_EQ(build(dir, kMonday, day), "");
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
    std::ofstream{store / "AAA.itch.zst", std::ios::app | std::ios::binary} << std::string(1'000, 't');  // a long tail
    write_file(store / "DDD.itch.zst", "a file it created");
    write_file(store / "index.tsv.tmp", "not an index");

    EXPECT_EQ(read_back(dir, kFriday, "AAA"), read_back(dir, kFriday, "AAA"));
    EXPECT_FALSE(read_back(dir, kFriday, "AAA").empty());
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
    write_file(day_file(dir, kMonday), {reinterpret_cast<const char*>(monday.bytes().data()), monday.bytes().size()});

    StoreBuilder builder{store, day_file(dir, kMonday), kMonday};
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
TEST(StoreBuilderTest, ADayFileChangedWhileItIsReadIsNotCommitted)
{
    const ScratchDir dir{"store-source"};
    const ItchBuilder day = three_symbols();
    for (const bool replace : {true, false}) {
        write_file(day_file(dir, kMonday), {reinterpret_cast<const char*>(day.bytes().data()), day.bytes().size()});
        StoreBuilder builder{dir.path / "store", day_file(dir, kMonday), kMonday};
        feed(builder, day.bytes());
        if (replace) {
            write_file(dir.path / "new", "another version");
            std::filesystem::rename(dir.path / "new", day_file(dir, kMonday));
        } else {
            std::ofstream{day_file(dir, kMonday), std::ios::app | std::ios::binary} << "more";
        }
        std::string error;
        EXPECT_FALSE(builder.commit(kSourceHash, error));
        EXPECT_EQ(error, "the day file changed while it was read");
        EXPECT_FALSE(std::filesystem::exists(dir.path / "store" / "index.tsv"));
        EXPECT_EQ(std::filesystem::file_size(dir.path / "store" / "AAA.itch.zst"), 0u);
    }
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
    write_file(day_file(dir, kFriday), "replaced");
    StoreBuilder failed{store, day_file(dir, kFriday), kFriday};
    feed(failed, changed.bytes());
    std::filesystem::rename(day_file(dir, kFriday), dir.path / "moved");
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
    EXPECT_EQ(read(shorter), "AAA on 2019-12-30: Destination buffer is too small");
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

}  // namespace
}  // namespace simulator::store_test
