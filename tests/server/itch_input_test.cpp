#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
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
