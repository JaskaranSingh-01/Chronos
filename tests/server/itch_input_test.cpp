#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "chronos/server/itch_input.hpp"
#include "chronos/server/replay_session.hpp"
#include "itch/itch_test_builder.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using namespace std::chrono_literals;

// The events `source` yields before it ends or fails.
std::size_t count(MarketEventSource& source)
{
    MarketEvent event;
    std::size_t events = 0;
    while (source.next(event) == SourceResult::Event) {
        ++events;
    }
    return events;
}

TEST(ItchInputTest, TheDirectoryListsTheTickersBeforeTheStartOfSystemHours)
{
    itch_test::ItchBuilder itch;
    itch.system_event(0, 1'000, 'O')  // start of messages: the directory follows
        .stock_directory(2, 2'000, "MSFT")
        .stock_directory(1, 2'000, "AAPL")
        .system_event(0, 3'000, 'S')  // start of system hours: the directory is complete
        .stock_directory(3, 4'000, "LATE");
    EXPECT_EQ(symbol_directory(itch.bytes()), (std::vector<std::string>{"AAPL", "MSFT"}));
    EXPECT_TRUE(symbol_directory({}).empty());
}

TEST(ItchInputTest, TheFingerprintNamesTheFileItsSizeAndItsFirstAndLastMebibyte)
{
    EXPECT_EQ(input_fingerprint("x", {}), "x 0 cbf29ce484222325");  // an empty file hashes to FNV-1a's offset basis
    std::vector<std::byte> day(3 << 20, std::byte{7});
    const std::string print = input_fingerprint("day.bin", day);
    EXPECT_EQ(print.substr(0, 16), "day.bin 3145728 ");
    EXPECT_EQ(print.size(), 32u);  // then 16 hex digits
    EXPECT_EQ(input_fingerprint("day.bin", day), print);
    day.back() = std::byte{8};
    const std::string last_changed = input_fingerprint("day.bin", day);
    EXPECT_NE(last_changed, print);
    day.front() = std::byte{8};
    EXPECT_NE(input_fingerprint("day.bin", day), last_changed);
}

TEST(ItchInputTest, TheDayHashIsXxh3Over128BitsOfEveryByteAndTheQuickCheckIsWeak)
{
    EXPECT_EQ(day_hash({}), "99aa06d3014798d86001c324468d497f");  // XXH3-128 of nothing, as xxhsum -H2 prints it
    std::vector<std::byte> day(3 << 20, std::byte{7});
    const std::string hash = day_hash(day);
    const std::uint64_t quick = quick_check(day);
    day[day.size() / 2] = std::byte{8};  // the middle, between the first and last MiB
    EXPECT_NE(day_hash(day), hash);
    EXPECT_EQ(quick_check(day), quick);
}

TEST(ItchInputTest, ALibraryFindsOnlyDayFilesWithARealDateOldestFirst)
{
    const ScratchDir dir{"days"};
    const itch_test::ItchBuilder day = acme_day();
    for (const char* name :
         {"12302019.NASDAQ_ITCH50.bin", "12272019.NASDAQ_ITCH50.bin", "12302019.NASDAQ_ITCH50.first-256MiB.bin",
          "12302019.NASDAQ_ITCH50.gz.partial", "02302019.NASDAQ_ITCH50.bin", "1230219.NASDAQ_ITCH50.bin",
          "12301969.NASDAQ_ITCH50.bin", "12302262.NASDAQ_ITCH50.bin", "notes.txt"}) {
        write_file(dir.path / name, day.bytes());
    }
    std::filesystem::create_directory(dir.path / "12312019.NASDAQ_ITCH50.bin");  // cannot be mapped: skipped
    DayLibrary library{dir.path, dir.path / "runs" / "fingerprints.json"};
    EXPECT_EQ(library.find(2019y / 12 / 30), nullptr);  // nothing before the first scan

    const std::vector<DayHandle> days = library.scan();
    ASSERT_EQ(days.size(), 2u);
    EXPECT_EQ(days[0]->date, 2019y / 12 / 27);
    EXPECT_EQ(days[1]->date, 2019y / 12 / 30);
    EXPECT_EQ(days[1]->file, "12302019.NASDAQ_ITCH50.bin");
    EXPECT_EQ(library.find(2019y / 12 / 30), days[1]);
    EXPECT_EQ(library.find(2019y / 12 / 31), nullptr);
    EXPECT_TRUE(std::filesystem::exists(dir.path / "runs" / "fingerprints.json"));
    EXPECT_EQ(library.scan(), days);  // nothing changed: the same handles

    std::filesystem::remove(dir.path / "12272019.NASDAQ_ITCH50.bin");
    EXPECT_EQ(library.scan(), (std::vector<DayHandle>{days[1]}));
    EXPECT_EQ(library.find(2019y / 12 / 27), nullptr);
    EXPECT_EQ(days[0]->mapped.bytes().size(), day.bytes().size());  // a handle keeps its day mapped
    EXPECT_TRUE(std::ranges::equal(days[0]->mapped.bytes(), day.bytes()));  // with its bytes, the file gone

    const auto cache_text = [&] {
        std::ifstream in{dir.path / "runs" / "fingerprints.json"};
        return std::string{std::istreambuf_iterator<char>{in}, {}};
    };
    const std::string cached = cache_text();
    ASSERT_FALSE(cached.empty());
    const std::filesystem::path away = dir.path.string() + "-away";
    std::filesystem::rename(dir.path, away);  // a directory that cannot be listed
    EXPECT_EQ(library.scan(), (std::vector<DayHandle>{days[1]}));
    std::filesystem::rename(away, dir.path);
    EXPECT_EQ(cache_text(), cached);  // leaves the days and the cache as they were
}

TEST(ItchInputTest, TheHashPassRecordsTheHashTheFeedSessionAndTheItchCheck)
{
    const ScratchDir dir{"identify"};
    itch_test::ItchBuilder early;  // regular hours from 09:30 to an early close at 13:00
    early.system_event(0, kSecond, 'O')
        .stock_directory(1, kSecond, "ACME")
        .system_event(0, 2 * kSecond, 'S')
        .system_event(0, kNineThirty, 'Q')
        .add_order(1, kNineThirty, 1, 'B', 100, "ACME", 100'000)
        .system_event(0, 13 * 3'600 * kSecond, 'M');
    itch_test::ItchBuilder no_hours;  // no 'Q' or 'M'
    no_hours.system_event(0, kSecond, 'O').stock_directory(1, kSecond, "ACME").system_event(0, 2 * kSecond, 'S');
    itch_test::ItchBuilder no_start;  // no start of messages first
    no_start.stock_directory(1, kSecond, "ACME").system_event(0, 2 * kSecond, 'O');
    const std::span<const std::byte> cut = early.bytes().first(early.bytes().size() - 3);
    std::vector<std::byte> broken(early.bytes().begin(), early.bytes().end());
    broken.insert(broken.begin() + 14, {std::byte{0}, std::byte{0}});  // a zero length after the 'O'
    write_file(dir.path / "12232019.NASDAQ_ITCH50.bin", early.bytes());
    write_file(dir.path / "12242019.NASDAQ_ITCH50.bin", no_hours.bytes());
    write_file(dir.path / "12262019.NASDAQ_ITCH50.bin", no_start.bytes());
    write_file(dir.path / "12272019.NASDAQ_ITCH50.bin", cut);
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", broken);
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    const std::vector<DayHandle> days = library.scan();
    ASSERT_EQ(days.size(), 5u);

    const Day& day = *days[0];
    const Timestamp midnight = session_midnight_ns(2019y / 12 / 23, -5h);
    EXPECT_EQ(day.midnight, midnight);
    EXPECT_EQ(day.size, early.bytes().size());
    EXPECT_EQ(day.hash, day_hash(early.bytes()));
    EXPECT_EQ(day.quick_check, quick_check(early.bytes()));
    EXPECT_TRUE(day.itch50);
    EXPECT_FALSE(day.cut_tail);
    EXPECT_EQ(day.session,
              (FeedSession{.open = midnight + RTH_OPEN_NS,
                           .close = midnight + 13 * 3'600 * kSecond,
                           .source = SessionSource::Feed}));
    EXPECT_EQ(day.symbols, std::vector<std::string>{"ACME"});

    const Timestamp midnight24 = session_midnight_ns(2019y / 12 / 24, -5h);
    EXPECT_EQ(days[1]->session,
              (FeedSession{.open = midnight24 + RTH_OPEN_NS,
                           .close = midnight24 + RTH_CLOSE_NS,
                           .source = SessionSource::Fallback}));
    EXPECT_TRUE(days[1]->itch50);
    EXPECT_FALSE(days[2]->itch50);
    EXPECT_TRUE(days[3]->itch50);  // a cut tail is noted; allow_truncated_tail decides if it replays
    EXPECT_TRUE(days[3]->cut_tail);
    EXPECT_EQ(days[3]->hash, day_hash(cut));
    EXPECT_FALSE(days[4]->itch50);  // its framing breaks mid-file
    EXPECT_FALSE(days[4]->cut_tail);
}

// A fresh library (a restarted server) takes an unchanged version from the cache, so a forged cached hash shows; a new
// mtime, inode, quick check or size is a new version, hashed again. A cache that does not parse is rebuilt.
TEST(ItchInputTest, AnUnchangedVersionComesFromTheCacheAndAnyChangeIsHashedAgain)
{
    const ScratchDir dir{"cache"};
    const std::filesystem::path file = dir.path / "12302019.NASDAQ_ITCH50.bin";
    const std::filesystem::path cache = dir.path / "runs" / "fingerprints.json";
    itch_test::ItchBuilder day = acme_day();
    write_file(file, day.bytes());
    const auto rescan = [&] { return DayLibrary{dir.path, cache}.scan().at(0)->hash; };
    const std::string hash = rescan();
    const std::string forged(32, '0');
    const auto forge = [&] {
        std::ifstream in{cache};
        std::string text{std::istreambuf_iterator<char>{in}, {}};
        text.replace(text.find("\"hash\":\"") + 8, 32, forged);
        std::ofstream{cache} << text;
    };

    forge();
    EXPECT_EQ(rescan(), forged);

    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(file) + 1s;
    std::filesystem::last_write_time(file, mtime);
    EXPECT_EQ(rescan(), hash);

    forge();
    std::filesystem::copy_file(file, dir.path / "copy");
    std::filesystem::last_write_time(dir.path / "copy", mtime);
    std::filesystem::rename(dir.path / "copy", file);  // the same name, size and mtime on a new inode
    EXPECT_EQ(rescan(), hash);

    forge();
    {
        std::fstream edit{file, std::ios::in | std::ios::out | std::ios::binary};
        edit.seekp(20);
        edit.put('x');  // in the first MiB, so the quick check changes
    }
    std::filesystem::last_write_time(file, mtime);
    const std::string edited = rescan();
    EXPECT_NE(edited, forged);
    EXPECT_NE(edited, hash);

    forge();
    day.system_event(0, 20 * 3'600 * kSecond, 'C');
    write_file(file, day.bytes());  // the end of messages: one more message
    std::filesystem::last_write_time(file, mtime);
    EXPECT_EQ(rescan(), day_hash(day.bytes()));

    std::ofstream{cache} << "not json";
    EXPECT_EQ(rescan(), day_hash(day.bytes()));
    EXPECT_EQ(rescan(), day_hash(day.bytes()));  // and rebuilt

    std::filesystem::remove(cache);
    std::filesystem::create_directory(cache);  // a cache that cannot be written leaves no temp file behind
    EXPECT_EQ(rescan(), day_hash(day.bytes()));
    EXPECT_FALSE(std::filesystem::exists(cache.string() + ".tmp"));
}

// Review gate 1, UI-2b's part: a mid-file edit that keeps the size, mtime and inode leaves the quick check as it was,
// so the listing still shows the cached hash; only a full re-hash of the file sees the edit.
TEST(ItchInputTest, AMidFileEditKeepingSizeMtimeAndInodeStillListsTheCachedHash)
{
    const ScratchDir dir{"midfile"};
    const std::filesystem::path file = dir.path / "12302019.NASDAQ_ITCH50.bin";
    itch_test::ItchBuilder day = acme_day();
    for (std::uint64_t match = 1; match <= 50'000; ++match) {  // 2.3 MB of prints
        day.trade(1, kNineThirty + 9 * kSecond, 0, 'B', 100, "ACME", 100'000, match);
    }
    write_file(file, day.bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    const DayHandle read = library.scan().at(0);  // in three steps, each cutting a frame
    const std::string hash = read->hash;
    EXPECT_EQ(hash, day_hash(day.bytes()));
    EXPECT_TRUE(read->itch50);
    EXPECT_FALSE(read->cut_tail);
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(file);
    {
        std::fstream edit{file, std::ios::in | std::ios::out | std::ios::binary};
        edit.seekp(static_cast<std::streamoff>(day.bytes().size() / 2));
        edit.put('x');
    }
    std::filesystem::last_write_time(file, mtime);

    EXPECT_EQ(library.scan().at(0)->hash, hash);
    MappedFile now;
    ASSERT_TRUE(now.open(file.c_str()));
    EXPECT_NE(day_hash(now.bytes()), hash);

    {
        std::fstream edit{file, std::ios::in | std::ios::out | std::ios::binary};
        edit.seekp(20);
        edit.put('y');  // in the first MiB: the quick check sees it
    }
    std::filesystem::last_write_time(file, mtime);
    const std::string rehashed = library.scan().at(0)->hash;  // the same library, holding the earlier version
    EXPECT_NE(rehashed, hash);
    MappedFile edited;
    ASSERT_TRUE(edited.open(file.c_str()));
    EXPECT_EQ(rehashed, day_hash(edited.bytes()));
}

TEST(ItchInputTest, AFileCutMidMessageFailsUnlessItsTailIsAllowed)
{
    itch_test::ItchBuilder itch;
    itch.stock_directory(1, 1, "AAPL")
        .add_order(1, 1'000, 1, 'B', 100, "AAPL", 2'900'000)
        .add_order(1, 2'000, 2, 'B', 100, "AAPL", 2'899'000);
    const std::span<const std::byte> cut = itch.bytes().first(itch.bytes().size() - 5);  // the last order is cut
    const std::unique_ptr<MarketEventSource> strict = itch_factory(cut, 2019y / 12 / 30, -5h, false)("AAPL");
    EXPECT_EQ(count(*strict), 1u);
    EXPECT_EQ(strict->error(), SourceError::TruncatedMessage);
    const std::unique_ptr<MarketEventSource> lenient = itch_factory(cut, 2019y / 12 / 30, -5h, true)("AAPL");
    EXPECT_EQ(count(*lenient), 1u);
    EXPECT_EQ(lenient->error(), SourceError::None);  // a clean end
}

TEST(ItchInputTest, TheFactoryReplaysOneTickerPerStreamAndASessionSwitchesBetweenThem)
{
    itch_test::ItchBuilder itch;
    itch.stock_directory(1, 1, "AAPL")
        .stock_directory(2, 1, "MSFT")
        .add_order(1, 1'000, 1, 'B', 100, "AAPL", 2'900'000)
        .add_order(2, 1'500, 2, 'S', 100, "MSFT", 1'500'000)
        .add_order(2, 1'600, 3, 'S', 100, "MSFT", 1'500'100)
        .erase(2, 1'700, 2)
        .erase(1, 2'000, 1);
    const SourceFactory factory = itch_factory(itch.bytes(), 2019y / 12 / 30, -5h, false);
    const std::unique_ptr<MarketEventSource> aapl = factory("AAPL");
    const std::unique_ptr<MarketEventSource> msft = factory("MSFT");  // each with its own registry
    EXPECT_EQ(count(*aapl), 2u);
    EXPECT_EQ(count(*msft), 3u);
    const std::unique_ptr<MarketEventSource> none = factory("");  // no symbol chosen: an empty stream
    EXPECT_EQ(count(*none), 0u);
    EXPECT_EQ(none->error(), SourceError::None);
    const std::unique_ptr<MarketEventSource> unknown = factory("NOPE");
    EXPECT_EQ(count(*unknown), 0u);
    EXPECT_EQ(unknown->error(), SourceError::SymbolNotFound);

    ReplaySession session{config(), factory};
    EXPECT_EQ(session.state(), SessionState::Ended);  // the first generation has no symbol
    session.control(Control{.type = ControlType::Symbol, .symbol = ticker("MSFT")});
    session.control(step(10));
    session.tick(kWall);
    EXPECT_EQ(session.generation(), 2u);
    EXPECT_EQ(session.loop_report().replay.events, 3u);
    EXPECT_EQ(name(latest(session).symbol), "MSFT");
}

}  // namespace
}  // namespace simulator::server::test
