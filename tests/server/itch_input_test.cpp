#include <gtest/gtest.h>

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "chronos/server/itch_input.hpp"
#include "chronos/server/replay_session.hpp"
#include "itch/itch_test_builder.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/itch/itch_source.hpp"
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

// Spec §3: a day lists the symbols its store holds a market event of, whenever they were listed; a symbol listed but
// without one has a segment, and is not listed.
TEST(ItchInputTest, ADayListsTheSymbolsWithMarketEventsInItsStore)
{
    const ScratchDir dir{"symbols"};
    itch_test::ItchBuilder itch;
    itch.system_event(0, 1'000, 'O')
        .stock_directory(2, 2'000, "MSFT")
        .stock_directory(1, 2'000, "AAPL")
        .add_order(1, 2'500, 1, 'B', 100, "AAPL", 2'900'000)
        .system_event(0, 3'000, 'S')
        .stock_directory(3, 4'000, "LATE")
        .trade(3, 4'500, 0, 'S', 100, "LATE", 100'000, 1);
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", itch.bytes());
    const DayHandle day = DayLibrary{dir.path, dir.path / "fingerprints.json"}.scan().at(0);
    EXPECT_EQ(day->symbols, (std::vector<std::string>{"AAPL", "LATE"}));
    ASSERT_NE(day->segment("MSFT"), nullptr);
    EXPECT_EQ(day->segment("MSFT")->events, 0u);
    EXPECT_EQ(day->segment("NOPE"), nullptr);
    EXPECT_EQ(day->store, dir.path / "store");
}

TEST(ItchInputTest, TheQuickCheckReadsTheFirstAndLastMebibyte)
{
    EXPECT_EQ(quick_check({}), fnv1a_offset_basis);
    std::vector<std::byte> day(3 << 20, std::byte{7});
    const std::uint64_t quick = quick_check(day);
    day.back() = std::byte{8};
    const std::uint64_t last_changed = quick_check(day);
    EXPECT_NE(last_changed, quick);
    day.front() = std::byte{8};
    EXPECT_NE(quick_check(day), last_changed);
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
    DaySequenceSource gone{{days[0]}, "ACME", kEquity, false, false};  // a handle keeps its segments, the file gone
    EXPECT_EQ(count(gone), 7u);
    EXPECT_EQ(gone.error(), SourceError::None);

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

// A fresh library (a restarted server) takes an unchanged version from the cache when the store holds its hash, so a
// forged hash shows; a new mtime, inode, quick check or size is a new version, read again. A cache that does not parse
// is rebuilt.
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
    // The cached hash, and the store's record of it, become `forged`: the version is current without a pass.
    const auto forge = [&] {
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream in{path};
            return std::string{std::istreambuf_iterator<char>{in}, {}};
        };
        std::string text = read(cache);
        const std::size_t at = text.find("\"hash\":\"") + 8;
        const std::string was = text.substr(at, 32);
        text.replace(at, 32, forged);
        std::ofstream{cache} << text;
        std::string index = read(dir.path / "store" / "index.tsv");
        index.replace(index.find("\t" + was + "\n"), 34, "\t" + forged + "\n");
        std::ofstream{dir.path / "store" / "index.tsv"} << index;
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

// The library's days in `dir`, each written from its builder under its date's file name.
std::vector<DayHandle> library_days(const ScratchDir& dir,
                                    std::initializer_list<std::pair<const char*, std::span<const std::byte>>> files)
{
    for (const auto& [name, bytes] : files) {
        write_file(dir.path / name, bytes);
    }
    return DayLibrary{dir.path, dir.path / "fingerprints.json"}.scan();
}

// Every event `source` yields before it ends or fails, and its position after each.
std::pair<std::vector<MarketEvent>, std::vector<std::uint64_t>> drain(MarketEventSource& source)
{
    std::vector<MarketEvent> events;
    std::vector<std::uint64_t> positions;
    MarketEvent event;
    while (source.next(event) == SourceResult::Event) {
        events.push_back(event);
        positions.push_back(source.position());
    }
    return {events, positions};
}

TEST(ItchInputTest, ADaySequenceReplaysItsDaysWithAClearAtEachNextMidnight)
{
    const ScratchDir dir{"chain"};
    const itch_test::ItchBuilder friday = acme_day(2);
    const itch_test::ItchBuilder monday = acme_day(3, 1'000);
    const std::vector<DayHandle> days = library_days(
        dir, {{"12272019.NASDAQ_ITCH50.bin", friday.bytes()}, {"12302019.NASDAQ_ITCH50.bin", monday.bytes()}});
    DaySequenceSource chain{days, "ACME", kEquity, false, false};
    EXPECT_EQ(chain.size(), friday.bytes().size() + monday.bytes().size());

    const auto [events, positions] = drain(chain);
    EXPECT_EQ(chain.error(), SourceError::None);
    ASSERT_EQ(events.size(), 12u);  // a trading state, two orders and two trades; the Clear; then three trades
    EXPECT_EQ(events[4].timestamp, session_midnight_ns(2019y / 12 / 27, -5h) + kNineThirty + 2 * kSecond);
    EXPECT_EQ(events[5].type, MarketEventType::Clear);
    EXPECT_EQ(events[5].timestamp, session_midnight_ns(2019y / 12 / 30, -5h));
    EXPECT_EQ(positions[5], friday.bytes().size());  // the Clear: all of Friday, none of Monday
    EXPECT_EQ(events[7].price, Price{101'000});      // Monday's bid
    EXPECT_GT(positions[6], friday.bytes().size());
    EXPECT_EQ(chain.position(), chain.size());
    EXPECT_TRUE(std::is_sorted(events.begin(), events.end(),
                               [](const MarketEvent& a, const MarketEvent& b) { return a.timestamp < b.timestamp; }));
    EXPECT_TRUE(std::all_of(events.begin(), events.end(), [](const MarketEvent& e) { return e.symbol == 0; }));

    DaySequenceSource one{{days[1]}, "ACME", kEquity, false, false};  // one day: no Clear at all
    const auto [alone, unused] = drain(one);
    EXPECT_EQ(alone.size(), 6u);
    EXPECT_TRUE(std::none_of(alone.begin(), alone.end(),
                             [](const MarketEvent& e) { return e.type == MarketEventType::Clear; }));
}

// Each day's times come from its own date's offset: Monday after the spring change runs on daylight time.
TEST(ItchInputTest, ADaySequenceAcrossADaylightSavingChangeStaysInOrder)
{
    const ScratchDir dir{"dst"};
    const itch_test::ItchBuilder day = acme_day(1);
    const std::vector<DayHandle> days =
        library_days(dir, {{"03082019.NASDAQ_ITCH50.bin", day.bytes()}, {"03112019.NASDAQ_ITCH50.bin", day.bytes()}});
    DaySequenceSource chain{days, "ACME", kEquity, false, false};
    const auto [events, positions] = drain(chain);
    ASSERT_EQ(events.size(), 9u);
    EXPECT_EQ(events[3].timestamp, session_midnight_ns(2019y / 3 / 8, -5h) + kNineThirty + kSecond);
    EXPECT_EQ(events[4].type, MarketEventType::Clear);
    EXPECT_EQ(events[4].timestamp, session_midnight_ns(2019y / 3 / 11, -4h));
    EXPECT_EQ(events[5].timestamp, session_midnight_ns(2019y / 3 / 11, -4h) + 3 * 3'600 * kSecond);
    EXPECT_TRUE(std::is_sorted(events.begin(), events.end(),
                               [](const MarketEvent& a, const MarketEvent& b) { return a.timestamp < b.timestamp; }));
}

// A day that fails ends the chain with its own error, after every event of the days before it.
TEST(ItchInputTest, AFailingDayFailsTheChainAfterTheDaysBeforeIt)
{
    const ScratchDir dir{"failing"};
    const itch_test::ItchBuilder friday = acme_day(2);
    const itch_test::ItchBuilder monday = acme_day(3);
    const std::span<const std::byte> cut = monday.bytes().first(monday.bytes().size() - 3);  // 'M' is cut
    const std::vector<DayHandle> days =
        library_days(dir, {{"12272019.NASDAQ_ITCH50.bin", friday.bytes()}, {"12302019.NASDAQ_ITCH50.bin", cut}});
    DaySequenceSource strict{days, "ACME", kEquity, false, false};
    const auto [events, positions] = drain(strict);
    EXPECT_EQ(events.size(), 12u);  // every event of both days: only the last message is cut
    EXPECT_EQ(strict.error(), SourceError::TruncatedMessage);
    EXPECT_GT(strict.position(), friday.bytes().size());  // in Monday

    DaySequenceSource lenient{days, "ACME", kEquity, true, false};
    EXPECT_EQ(drain(lenient).first.size(), 12u);
    EXPECT_EQ(lenient.error(), SourceError::None);
}

// Hash as read keeps each segment's hash as read, a cut tail included.
TEST(ItchInputTest, HashAsReadKeepsEachSegmentsHashIncludingACutTail)
{
    const ScratchDir dir{"hash-as-read"};
    const itch_test::ItchBuilder friday = acme_day(2);
    const itch_test::ItchBuilder monday = acme_day(3);
    const std::span<const std::byte> cut = monday.bytes().first(monday.bytes().size() - 3);
    const std::vector<DayHandle> days =
        library_days(dir, {{"12272019.NASDAQ_ITCH50.bin", friday.bytes()}, {"12302019.NASDAQ_ITCH50.bin", cut}});
    DaySequenceSource chain{days, "ACME", kEquity, true, true};
    EXPECT_TRUE(chain.hashes().empty());
    drain(chain);
    EXPECT_EQ(chain.hashes(),
              (std::vector<std::string>{days[0]->segment("ACME")->hash, days[1]->segment("ACME")->hash}));
    EXPECT_EQ(days[1]->segment("ACME")->hash, day_hash(cut));  // ACME's frames and the System Events: the whole day
    EXPECT_TRUE(days[1]->cut_tail);

    DaySequenceSource unhashed{days, "ACME", kEquity, true, false};
    drain(unhashed);
    EXPECT_TRUE(unhashed.hashes().empty());
}

TEST(ItchInputTest, AChainWithADayNotItch50OrOtherInstrumentFieldsIsRefusedBeforeItsFirstEvent)
{
    const ScratchDir dir{"refused"};
    itch_test::ItchBuilder no_start;  // no start of messages first
    no_start.stock_directory(1, kSecond, "ACME").add_order(1, kNineThirty, 1, 'B', 100, "ACME", 100'000);
    const std::vector<DayHandle> days = library_days(
        dir, {{"12272019.NASDAQ_ITCH50.bin", acme_day().bytes()}, {"12302019.NASDAQ_ITCH50.bin", no_start.bytes()}});
    MarketEvent event;
    DaySequenceSource not_itch{days, "ACME", kEquity, false, false};
    EXPECT_EQ(not_itch.next(event), SourceResult::Error);
    EXPECT_EQ(not_itch.error(), SourceError::InvalidHeader);
    EXPECT_EQ(not_itch.position(), 0u);

    DaySequenceSource cents{{days[0]}, "ACME", Instrument{.price_scale = 2, .quantity_scale = 0}, false, false};
    EXPECT_EQ(cents.next(event), SourceResult::Error);
    EXPECT_EQ(cents.error(), SourceError::InstrumentRejected);
    EXPECT_EQ(cents.next(event), SourceResult::Error);  // sticky
}

TEST(ItchInputTest, AFileCutMidMessageFailsUnlessItsTailIsAllowed)
{
    const ScratchDir dir{"cut"};
    const itch_test::ItchBuilder day = acme_day(2);
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", day.bytes().first(day.bytes().size() - 5));  // 'M' is cut
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    library.scan();
    const DayList days{.day = {2019y / 12 / 30}, .count = 1};
    const std::unique_ptr<MarketEventSource> strict = chain_factory(library, kEquity, false)(ticker("ACME"), days);
    EXPECT_EQ(count(*strict), 5u);
    EXPECT_EQ(strict->error(), SourceError::TruncatedMessage);
    const std::unique_ptr<MarketEventSource> lenient = chain_factory(library, kEquity, true)(ticker("ACME"), days);
    EXPECT_EQ(count(*lenient), 5u);
    EXPECT_EQ(lenient->error(), SourceError::None);  // a clean end
}

TEST(ItchInputTest, TheChainFactoryReplaysOneTickerPerStreamAndASessionSwitchesBetweenThem)
{
    itch_test::ItchBuilder itch;
    itch.system_event(0, 1, 'O')
        .stock_directory(1, 1, "AAPL")
        .stock_directory(2, 1, "MSFT")
        .add_order(1, 1'000, 1, 'B', 100, "AAPL", 2'900'000)
        .add_order(2, 1'500, 2, 'S', 100, "MSFT", 1'500'000)
        .add_order(2, 1'600, 3, 'S', 100, "MSFT", 1'500'100)
        .erase(2, 1'700, 2)
        .erase(1, 2'000, 1);
    const ScratchDir dir{"factory"};
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", itch.bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    library.scan();
    const DayList days{.day = {2019y / 12 / 30}, .count = 1};
    const SourceFactory factory = chain_factory(library, kEquity, false);
    const std::unique_ptr<MarketEventSource> aapl = factory(ticker("AAPL"), days);
    const std::unique_ptr<MarketEventSource> msft = factory(ticker("MSFT"), days);  // each with its own registry
    EXPECT_EQ(count(*aapl), 2u);
    EXPECT_EQ(count(*msft), 3u);
    const std::unique_ptr<MarketEventSource> none = factory(Ticker{}, days);  // no symbol chosen: an empty stream
    EXPECT_EQ(count(*none), 0u);
    EXPECT_EQ(none->error(), SourceError::None);
    const std::unique_ptr<MarketEventSource> unknown = factory(ticker("NOPE"), days);
    EXPECT_EQ(count(*unknown), 0u);
    EXPECT_EQ(unknown->error(), SourceError::SymbolNotFound);
    const std::unique_ptr<MarketEventSource> gone = factory(ticker("AAPL"), {.day = {2019y / 12 / 31}, .count = 1});
    EXPECT_EQ(count(*gone), 0u);  // a day the library does not hold

    ReplaySession session{config(), factory};
    EXPECT_EQ(session.state(), SessionState::Ended);  // the first generation has no symbol
    session.control(Control{.type = ControlType::Symbol, .spec = {.ticker = ticker("MSFT"), .days = days}});
    session.control(step(10));
    session.tick(kWall);
    EXPECT_EQ(session.generation(), 2u);
    EXPECT_EQ(session.loop_report().replay.events, 3u);
    EXPECT_EQ(name(latest(session).spec.ticker), "MSFT");
}

// Spec §3: a segment that does not read back intact stops the chain with a store error, said on stderr, never with
// bytes; with hash as read, its hash as read is "".
TEST(ItchInputTest, ASegmentThatDoesNotReadBackStopsTheChainWithAStoreError)
{
    const ScratchDir dir{"damaged"};
    const std::vector<DayHandle> days = library_days(
        dir,
        {{"12272019.NASDAQ_ITCH50.bin", acme_day(2).bytes()}, {"12302019.NASDAQ_ITCH50.bin", acme_day(3).bytes()}});
    const Segment monday = *days[1]->segment("ACME");
    flip(dir.path / "store" / "ACME.itch.zst", monday.offset + monday.compressed - 1);  // its zstd checksum
    DaySequenceSource chain{days, "ACME", kEquity, false, true};
    EXPECT_EQ(drain(chain).first.size(), 6u);  // Friday's five events, then its Clear
    EXPECT_EQ(chain.error(), SourceError::StoreRead);
    EXPECT_EQ(chain.hashes(), (std::vector<std::string>{days[0]->segment("ACME")->hash, ""}));
    EXPECT_EQ(segment_hash_as_read(*days[0], "ACME"), days[0]->segment("ACME")->hash);
    EXPECT_EQ(segment_hash_as_read(*days[1], "ACME"), "");
    EXPECT_EQ(segment_hash_as_read(*days[0], "NOPE"), "");
}

// Spec §2.2: chronos-serve never repairs an invalid index. It moves it aside as index.tsv.bad, removes the symbol
// files, says so on stderr, and builds every day again.
TEST(ItchInputTest, AnInvalidStoreIndexIsMovedAsideAndEveryDayIsBuiltAgain)
{
    const ScratchDir dir{"invalid-index"};
    const std::vector<DayHandle> days = library_days(dir, {{"12302019.NASDAQ_ITCH50.bin", acme_day().bytes()}});
    const std::filesystem::path store = dir.path / "store";
    std::ofstream{store / "index.tsv", std::ios::app} << "not a record\n";
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    EXPECT_TRUE(std::filesystem::exists(store / "index.tsv.bad"));
    EXPECT_FALSE(std::filesystem::exists(store / "index.tsv"));
    EXPECT_FALSE(std::filesystem::exists(store / "ACME.itch.zst"));
    const DayHandle rebuilt = library.scan().at(0);
    EXPECT_EQ(*rebuilt->segment("ACME"), *days[0]->segment("ACME"));
    EXPECT_EQ(library.usage().unreferenced, 0u);
    EXPECT_EQ(library.usage().bytes, rebuilt->segment("ACME")->compressed);
}

// A version read again whose hash the store already holds, as after a touch, is current: nothing is written, not even
// the 4 MiB flushes a build of ACME's 4.6 MB would make.
TEST(ItchInputTest, AVersionWhoseHashTheStoreHoldsIsNotBuiltAgain)
{
    const ScratchDir dir{"touched"};
    const std::filesystem::path file = dir.path / "12302019.NASDAQ_ITCH50.bin";
    const std::filesystem::path acme = dir.path / "store" / "ACME.itch.zst";
    itch_test::ItchBuilder day = acme_day();
    for (std::uint64_t match = 1; match <= 100'000; ++match) {
        day.trade(1, kNineThirty + 9 * kSecond, 0, 'B', 100, "ACME", 100'000, match);
    }
    write_file(file, day.bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    const DayHandle first = library.scan().at(0);
    const std::uintmax_t size = std::filesystem::file_size(acme);
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(acme) - 1h;
    std::filesystem::last_write_time(acme, written);
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + 1s);
    const DayHandle touched = library.scan().at(0);
    EXPECT_NE(touched, first);  // a new version, read again
    EXPECT_EQ(*touched->segment("ACME"), *first->segment("ACME"));
    EXPECT_EQ(std::filesystem::file_size(acme), size);
    EXPECT_EQ(std::filesystem::last_write_time(acme), written);
    EXPECT_EQ(library.usage().unreferenced, 0u);
}

// Spec §2.4: where a day's framing breaks mid-file, at a zero length, ACME's segment ends as the day file does: the same
// events, then InvalidLength.
TEST(ItchInputTest, ASegmentOfADayWhoseFramingBreaksEndsWithTheDayFilesEventsAndError)
{
    const ScratchDir dir{"broken"};
    const itch_test::ItchBuilder whole = acme_day(4);
    std::vector<std::byte> broken(whole.bytes().begin(), whole.bytes().end());
    broken.insert(broken.begin() + static_cast<std::ptrdiff_t>(acme_day(2).bytes().size() - 14),  // before its 'M'
                  {std::byte{0}, std::byte{0}});
    const DayHandle day = library_days(dir, {{"12302019.NASDAQ_ITCH50.bin", broken}}).at(0);
    std::vector<std::byte> segment;
    std::string error;
    ASSERT_NE(day->segment("ACME"), nullptr);
    ASSERT_TRUE(read_segment(day->store, "ACME", day->date, *day->segment("ACME"), segment, error)) << error;
    const auto stream = [&](std::span<const std::byte> bytes) {
        InstrumentRegistry registry{1};
        ItchSource source{bytes,
                          {.ticker = "ACME", .session_date = day->date, .utc_offset = us_eastern_utc_offset(day->date)},
                          registry};
        std::vector<std::pair<Timestamp, MarketEventType>> events;
        MarketEvent event;
        while (source.next(event) == SourceResult::Event) {
            events.emplace_back(event.timestamp, event.type);
        }
        return std::pair{events, source.error()};
    };
    EXPECT_EQ(stream(segment), stream(broken));
    EXPECT_EQ(stream(broken).first.size(), 5u);  // the trading state, two orders and two executions
    EXPECT_EQ(stream(broken).second, SourceError::InvalidLength);
}

// Spec §2.5 and §6, recovery: a build that fails at a write, here under RLIMIT_FSIZE, leaves its day unlisted and the
// days built before as they were; the next scan tries it again.
TEST(ItchInputTest, ADayWhoseStoreBuildFailsIsUnlistedUntilItBuilds)
{
    const ScratchDir dir{"build-fails"};
    write_file(dir.path / "12272019.NASDAQ_ITCH50.bin", acme_day(2).bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    const DayHandle friday = library.scan().at(0);
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", acme_day(40).bytes());
    rlimit was{};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &was), 0);
    const auto ignored = std::signal(SIGXFSZ, SIG_IGN);  // so a write past the limit fails with EFBIG
    rlimit low = was;
    low.rlim_cur = friday->segment("ACME")->compressed + 16;
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &low), 0);
    const std::vector<DayHandle> listed = library.scan();
    ::setrlimit(RLIMIT_FSIZE, &was);
    std::signal(SIGXFSZ, ignored);

    EXPECT_EQ(listed, std::vector<DayHandle>{friday});
    EXPECT_EQ(library.usage().unreferenced, 0u);
    EXPECT_EQ(segment_hash_as_read(*friday, "ACME"), friday->segment("ACME")->hash);
    ASSERT_EQ(library.scan().size(), 2u);
    EXPECT_EQ(library.find(2019y / 12 / 30)->segment("ACME")->offset, friday->segment("ACME")->compressed);
}

// Spec §2.5, concurrency: scans run one at a time, so of two at once the second finds the days the first built.
TEST(ItchInputTest, ConcurrentScansBuildADayOnce)
{
    const ScratchDir dir{"concurrent"};
    write_file(dir.path / "12272019.NASDAQ_ITCH50.bin", acme_day(2).bytes());
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", acme_day(3).bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    std::vector<DayHandle> other;
    std::thread scanning{[&] { other = library.scan(); }};
    const std::vector<DayHandle> days = library.scan();
    scanning.join();
    EXPECT_EQ(days.size(), 2u);
    EXPECT_EQ(other, days);  // the same handles: one pass over each day
    EXPECT_EQ(library.usage().unreferenced, 0u);
}

// Spec §2.5: one chronos-serve per store; the lock goes with its holder.
TEST(ItchInputTest, AStoreIsLockedByOneHolderAtATime)
{
    const ScratchDir dir{"lock"};
    {
        const StoreLock first{dir.path / "store"};
        EXPECT_EQ(first.error(), "");
        const StoreLock second{dir.path / "store"};
        EXPECT_EQ(second.error(), "in use by another chronos-serve");
    }
    const StoreLock again{dir.path / "store"};
    EXPECT_EQ(again.error(), "");
    std::ofstream{dir.path / "file"} << "not a directory";
    EXPECT_EQ(StoreLock{dir.path / "file" / "store"}.error(), "Not a directory");
}

}  // namespace
}  // namespace simulator::server::test
