#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "simulator/csv/csv_source.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/market/market_replay.hpp"

namespace simulator {
namespace {

// 2019-12-30 00:00 at -05:00, as epoch ns.
constexpr Timestamp midnight = 1'577'682'000'000'000'000;

constexpr std::string_view header6 = "timestamp,event_type,order_id,side,price,quantity";
constexpr std::string_view header7 = "timestamp,event_type,order_id,side,price,quantity,new_order_id";

std::span<const std::byte> bytes(std::string_view text)
{
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::string data_path(std::string_view file)
{
    return std::string{CHRONOS_TEST_DATA_DIR} + "/" + std::string{file};
}

auto fields(const MarketEvent& e)
{
    return std::tuple{e.timestamp,        e.order_id, e.new_order_id,           e.price.ticks,
                      e.quantity.value(), e.symbol,   static_cast<int>(e.type), static_cast<int>(e.side)};
}

// Reads events until the stream ends; the end result goes to `end`.
std::vector<MarketEvent> drain(CsvSource& source, SourceResult& end)
{
    std::vector<MarketEvent> events;
    MarketEvent event;

    while ((end = source.next(event)) == SourceResult::Event) {
        events.push_back(event);
    }
    return events;
}

// The source has ended in `error` at `position`, and stays there.
void expect_sticky_error(CsvSource& source, SourceError error, std::uint64_t position)
{
    MarketEvent event;

    for (int call = 0; call < 2; ++call) {
        EXPECT_EQ(source.next(event), SourceResult::Error) << "call " << call;
        EXPECT_EQ(source.error(), error) << "call " << call;
        EXPECT_EQ(source.position(), position) << "call " << call;
    }
}

TEST(CsvSourceTest, ReadsAddEvent)
{
    MappedFile file;
    ASSERT_TRUE(file.open(data_path("basic_replay.csv").c_str())) << file.error();

    CsvSource source{file.bytes(), 3};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(fields(event),
              fields(MarketEvent{.timestamp = 100,
                                 .order_id = 1,
                                 .price = Price{100},
                                 .quantity = Quantity{100},
                                 .symbol = 3,
                                 .type = MarketEventType::Add,
                                 .side = Side::Sell}));
    EXPECT_EQ(source.position(), 2u);
}

TEST(CsvSourceTest, ReadsMultipleEvents)
{
    MappedFile file;
    ASSERT_TRUE(file.open(data_path("basic_replay.csv").c_str())) << file.error();

    CsvSource source{file.bytes(), 3};
    SourceResult end{};
    const auto events = drain(source, end);

    ASSERT_EQ(events.size(), 4u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(events[i].type, MarketEventType::Add) << "event " << i;
        EXPECT_EQ(events[i].order_id, i + 1) << "event " << i;
    }

    // The row says BUY and 99; a Cancel names neither.
    EXPECT_EQ(
        fields(events[3]),
        fields(MarketEvent{
            .timestamp = 400, .order_id = 3, .quantity = Quantity{50}, .symbol = 3, .type = MarketEventType::Cancel}));

    // The file has no final newline: its last line is 5.
    MarketEvent event;
    EXPECT_EQ(end, SourceResult::EndOfStream);
    EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
    EXPECT_EQ(source.position(), 5u);
    EXPECT_EQ(source.error(), SourceError::None);
}

TEST(CsvSourceTest, ReadsQuantityAboveUint32Max)
{
    MappedFile file;
    ASSERT_TRUE(file.open(data_path("large_quantity_replay.csv").c_str())) << file.error();

    CsvSource source{file.bytes(), 0};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(event.quantity, Quantity{4294967296ULL});
}

TEST(CsvSourceTest, RejectsMalformedRow)
{
    MappedFile file;
    ASSERT_TRUE(file.open(data_path("malformed_replay.csv").c_str())) << file.error();

    CsvSource source{file.bytes(), 0};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Event);
    expect_sticky_error(source, SourceError::InvalidNumber, 3);
}

// "-1" is out of range, not UINT64_MAX as std::stoull would read it.
TEST(CsvSourceTest, RejectsQuantityOutOfRange)
{
    for (const std::string_view quantity : {"-1", "18446744073709551616"}) {
        SCOPED_TRACE(quantity);
        const std::string text = std::string{header6} + "\n100,ADD,1,BUY,1000000," + std::string{quantity} + "\n";

        CsvSource source{bytes(text), 0};
        expect_sticky_error(source, SourceError::NumberOutOfRange, 2);
    }

    // UINT64_MAX fits. The span ends at its last digit, but the memory after
    // it holds another '9', as an mmap need not end in NUL: a parser reading
    // past the span would see 2^64 * 10 - 1 and report it out of range.
    const std::string text = std::string{header6} + "\n100,ADD,1,BUY,1000000,18446744073709551615" + "9";
    CsvSource source{bytes(std::string_view{text}.substr(0, text.size() - 1)), 0};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(event.quantity, Quantity{std::numeric_limits<std::uint64_t>::max()});

    // The int64 columns stop at INT64_MAX instead.
    const std::string int64_max = std::string{header6} + "\n9223372036854775807,ADD,1,BUY,9223372036854775807,1\n";
    CsvSource at_max{bytes(int64_max), 0};

    ASSERT_EQ(at_max.next(event), SourceResult::Event);
    EXPECT_EQ(event.timestamp, std::numeric_limits<Timestamp>::max());
    EXPECT_EQ(event.price, Price{std::numeric_limits<std::int64_t>::max()});
}

// Columns an action ignores come out as 0 (or Buy).
TEST(CsvSourceTest, ReadsEveryEventType)
{
    const std::string text = std::string{header7} +
        "\n"
        "100,ADD,1,BUY,1000000,500,\n"
        "200,EXECUTE,1,,,100,\n"
        "300,EXECUTE,1,SELL,1000100,50,\n"
        "400,CANCEL,1,BUY,999900,50,\n"
        "500,REPLACE,1,SELL,1000200,300,2\n"
        "600,TRADE,7,SELL,1000300,25,\n"
        "700,TRADE,,BUY,1000400,10,\n"
        "800,DELETE,2,BUY,1000200,300,9\n";

    CsvSource source{bytes(text), 5};
    SourceResult end{};
    const auto events = drain(source, end);

    using T = MarketEventType;
    const MarketEvent expected[] = {
        {.timestamp = 100,
         .order_id = 1,
         .price = Price{1'000'000},
         .quantity = Quantity{500},
         .symbol = 5,
         .type = T::Add,
         .side = Side::Buy},
        {.timestamp = 200, .order_id = 1, .quantity = Quantity{100}, .symbol = 5, .type = T::Execute},
        {.timestamp = 300,
         .order_id = 1,
         .price = Price{1'000'100},
         .quantity = Quantity{50},
         .symbol = 5,
         .type = T::Execute},
        {.timestamp = 400, .order_id = 1, .quantity = Quantity{50}, .symbol = 5, .type = T::Cancel},
        {.timestamp = 500,
         .order_id = 1,
         .new_order_id = 2,
         .price = Price{1'000'200},
         .quantity = Quantity{300},
         .symbol = 5,
         .type = T::Replace},
        {.timestamp = 600,
         .order_id = 7,
         .price = Price{1'000'300},
         .quantity = Quantity{25},
         .symbol = 5,
         .type = T::Trade,
         .side = Side::Sell},
        {.timestamp = 700,
         .order_id = 0,
         .price = Price{1'000'400},
         .quantity = Quantity{10},
         .symbol = 5,
         .type = T::Trade,
         .side = Side::Buy},
        {.timestamp = 800, .order_id = 2, .symbol = 5, .type = T::Delete},
    };

    EXPECT_EQ(end, SourceResult::EndOfStream);
    EXPECT_EQ(source.position(), 9u);
    ASSERT_EQ(events.size(), std::size(expected));
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(fields(events[i]), fields(expected[i])) << "event " << i;
    }
}

TEST(CsvSourceTest, AcceptsCrlfAndBlankLines)
{
    const std::string text = "\r\n" + std::string{header7} +
        "\r\n"
        "\r\n"
        "100,ADD,1,BUY,100,10,\r\n"
        "\n"
        "200,DELETE,1,,,,\r\n";

    CsvSource source{bytes(text), 0};
    MarketEvent event;

    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(source.position(), 4u);
    EXPECT_EQ(fields(event),
              fields(MarketEvent{.timestamp = 100, .order_id = 1, .price = Price{100}, .quantity = Quantity{10}}));

    ASSERT_EQ(source.next(event), SourceResult::Event);
    EXPECT_EQ(source.position(), 6u);
    EXPECT_EQ(fields(event), fields(MarketEvent{.timestamp = 200, .order_id = 1, .type = MarketEventType::Delete}));

    EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
    EXPECT_EQ(source.position(), 6u);
}

TEST(CsvSourceTest, ChecksTheHeader)
{
    for (const std::string_view header : {header6, header7}) {
        SCOPED_TRACE(header);
        const std::string text = std::string{header} + "\n";
        CsvSource source{bytes(text), 0};
        MarketEvent event;

        EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
        EXPECT_EQ(source.next(event), SourceResult::EndOfStream);
        EXPECT_EQ(source.position(), 1u);
    }

    {
        SCOPED_TRACE("empty input");
        CsvSource source{bytes(""), 0};
        expect_sticky_error(source, SourceError::InvalidHeader, 0);
    }
    {
        SCOPED_TRACE("blank lines only");
        CsvSource source{bytes("\n\r\n"), 0};
        expect_sticky_error(source, SourceError::InvalidHeader, 2);
    }

    const std::string bad_headers[] = {
        "timestamp,event_type,order_id,side,price",
        std::string{header7} + ",extra",
        "Timestamp,event_type,order_id,side,price,quantity",
        std::string{header6} + " ",
        "\xEF\xBB\xBF" + std::string{header6},  // a UTF-8 BOM
        "100,ADD,1,BUY,100,10",
    };

    for (const std::string& header : bad_headers) {
        SCOPED_TRACE(header);
        const std::string text = header + "\n100,ADD,1,BUY,100,10\n";
        CsvSource source{bytes(text), 0};
        expect_sticky_error(source, SourceError::InvalidHeader, 1);
    }
}

TEST(CsvSourceTest, ReportsEachRowErrorWithItsLine)
{
    struct Case
    {
        std::string_view header;
        std::string_view row;
        SourceError error;
    };

    using E = SourceError;
    const Case cases[] = {
        {header6, "100,ADD,1,BUY,100", E::InvalidRow},
        {header6, "100,ADD,1,BUY,100,10,", E::InvalidRow},  // a trailing comma is a 7th column
        {header7, "100,ADD,1,BUY,100,10,,1", E::InvalidRow},
        {header7, "100,ADD,1,BUY,100,10", E::InvalidRow},
        {header6, "x,MODIFY,-1,HOLD,+1", E::InvalidRow},           // columns first,
        {header6, "x,MODIFY,-1,HOLD,+1,10", E::InvalidNumber},     // then column order
        {header6, "100,MODIFY,-1,HOLD,+1,10", E::InvalidEventType},
        {header6, "100,ADD,-1,HOLD,+1,10", E::NumberOutOfRange},
        {header6, "100,MODIFY,1,BUY,100,10", E::InvalidEventType},
        {header6, "100,add,1,BUY,100,10", E::InvalidEventType},
        {header6, "100,ADD,1,HOLD,100,10", E::InvalidSide},
        {header6, "100,ADD,1,,100,10", E::InvalidSide},
        {header6, "100,TRADE,1,,100,10", E::InvalidSide},
        {header6, "100,CANCEL,1,XXX,,10", E::InvalidSide},  // unused, but present
        {header6, "100,ADD,1,BUY,,10", E::InvalidNumber},
        {header6, "100,ADD,1,BUY,100abc,10", E::InvalidNumber},
        {header6, "100,ADD,1,BUY, 100,10", E::InvalidNumber},
        {header6, "100,ADD,1,BUY,+100,10", E::InvalidNumber},
        {header6, "100,ADD,1,BUY,100.5,10", E::InvalidNumber},
        {header6, "100,ADD,1,BUY,-,10", E::InvalidNumber},
        {header6, "100,ADD,1,BUY,99999999999999999999x,10", E::InvalidNumber},  // not range
        {header6, ",ADD,1,BUY,100,10", E::InvalidNumber},
        {header6, "100,DELETE,1,,,abc", E::InvalidNumber},
        {header6, "100,REPLACE,1,,100,10", E::InvalidNumber},  // no new_order_id column
        {header7, "100,REPLACE,1,,100,10,", E::InvalidNumber},
        {header6, "100,ADD,1,BUY,-100,10", E::NumberOutOfRange},
        {header6, "9223372036854775808,ADD,1,BUY,100,10", E::NumberOutOfRange},
        {header6, "100,ADD,-1,BUY,100,10", E::NumberOutOfRange},
    };

    for (const Case& c : cases) {
        SCOPED_TRACE(c.row);
        const std::string text = std::string{c.header} + "\n" + std::string{c.row} + "\n";
        CsvSource source{bytes(text), 0};
        expect_sticky_error(source, c.error, 2);
    }
}

TEST(CsvSourceTest, ReplaysBasicFileThroughMarketReplay)
{
    MappedFile file;
    ASSERT_TRUE(file.open(data_path("basic_replay.csv").c_str())) << file.error();

    CsvSource source{file.bytes(), 3};
    MarketBook book{3, 16};
    const ReplayReport report = MarketReplay::run(source, book, {});

    ASSERT_TRUE(report.succeeded()) << report.message;
    EXPECT_EQ(report.events, 4u);
    EXPECT_EQ(report.by_type[static_cast<std::size_t>(MarketEventType::Add)], 3u);
    EXPECT_EQ(report.by_type[static_cast<std::size_t>(MarketEventType::Cancel)], 1u);
    EXPECT_EQ(report.rejected(), 0u);
    EXPECT_EQ(report.last_timestamp, 400);

    // Historical Adds that cross both rest: the book never matches.
    EXPECT_EQ(book.find(3), nullptr);
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_TRUE(book.is_crossed());
}

TEST(CsvSourceTest, MarketReplayStopsAtTheMalformedLine)
{
    MappedFile file;
    ASSERT_TRUE(file.open(data_path("malformed_replay.csv").c_str())) << file.error();

    CsvSource source{file.bytes(), 3};
    MarketBook book{3, 16};
    const ReplayReport report = MarketReplay::run(source, book, {});

    EXPECT_EQ(report.failure, ReplayFailure::SourceError);
    EXPECT_EQ(report.source_error, SourceError::InvalidNumber);
    EXPECT_EQ(report.message, "InvalidNumber");
    EXPECT_EQ(report.failed_position, 3u);
    EXPECT_EQ(report.events, 1u);
    EXPECT_EQ(report.applied(), 1u);
}

// The book is checked after every event in RTH, where a crossed book fails the run.
TEST(CsvSourceTest, ReplaysEveryEventTypeWithZeroRejections)
{
    MappedFile file;
    ASSERT_TRUE(file.open(data_path("conformance/basic.csv").c_str())) << file.error();

    CsvSource source{file.bytes(), 0};
    MarketBook book{0, 16};
    const ReplayReport report =
        MarketReplay::run(source, book, {.check_invariants_every = 1, .session_midnight = midnight});

    ASSERT_TRUE(report.succeeded()) << report.message << " at line " << report.failed_position;
    EXPECT_EQ(report.events, 10u);
    EXPECT_EQ(report.rejected(), 0u);
    EXPECT_EQ(report.invariant_checks, 10u);

    using T = MarketEventType;
    const auto count = [&](T type) { return report.by_type[static_cast<std::size_t>(type)]; };
    EXPECT_EQ(count(T::Add), 3u);
    EXPECT_EQ(count(T::Execute), 3u);
    EXPECT_EQ(count(T::Cancel), 1u);
    EXPECT_EQ(count(T::Replace), 1u);
    EXPECT_EQ(count(T::Trade), 1u);
    EXPECT_EQ(count(T::Delete), 1u);

    LevelInfo level;
    EXPECT_EQ(book.order_count(), 1u);
    ASSERT_TRUE(book.best(Side::Buy, level));
    EXPECT_EQ(level, (LevelInfo{Price{999'900}, 400, 1}));
    EXPECT_FALSE(book.best(Side::Sell, level));
}

} // namespace
} // namespace simulator
