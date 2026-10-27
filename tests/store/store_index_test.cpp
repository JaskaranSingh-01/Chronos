#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "simulator/store/store_index.hpp"
#include "store_test_support.hpp"

namespace simulator::store_test {
namespace {

using namespace std::chrono_literals;

const std::string kHash = "0123456789abcdef0123456789abcdef";

// Two days: AAPL on both, BRK.B and the escaped A%B on the 30th.
StoreIndex two_days()
{
    StoreIndex index;
    index.set(2019y / 12 / 27,
              {.file = "12272019.NASDAQ_ITCH50.bin",
               .size = 1'000,
               .hash = kHash,
               .segments = {
                   {"AAPL",
                    {.offset = 0, .compressed = 40, .uncompressed = 90, .messages = 7, .events = 5, .hash = kHash}}}});
    index.set(
        2019y / 12 / 30,
        {.file = "12302019.NASDAQ_ITCH50.bin",
         .size = 2'000,
         .hash = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
         .segments = {
             {"A%B", {.offset = 0, .compressed = 9, .uncompressed = 12, .messages = 1, .events = 0, .hash = kHash}},
             {"AAPL", {.offset = 40, .compressed = 30, .uncompressed = 80, .messages = 6, .events = 4, .hash = kHash}},
             {"BRK.B",
              {.offset = 0, .compressed = 20, .uncompressed = 50, .messages = 3, .events = 1, .hash = kHash}}}});
    return index;
}

// Writes `index` into `dir` with each symbol file `extra` bytes longer than its last segment.
void write_store(const std::filesystem::path& dir, const std::string& text, std::size_t extra = 0)
{
    write_file(dir / "index.tsv", text);
    write_file(dir / "AAPL.itch.zst", std::string(70 + extra, 'z'));
    write_file(dir / "BRK.B.itch.zst", std::string(20 + extra, 'z'));
    write_file(dir / "A%25B.itch.zst", std::string(9 + extra, 'z'));
}

// Lower case is escaped too: on a case-insensitive file system, AAPL and aapl would share one file.
TEST(StoreIndexTest, SymbolFilesKeepCapitalsDigitsAndDotPlusEqualsMinusAndEscapeTheRest)
{
    EXPECT_EQ(symbol_file("AAPL"), "AAPL.itch.zst");
    EXPECT_EQ(symbol_file("BRK.B"), "BRK.B.itch.zst");
    EXPECT_EQ(symbol_file("a+=-9"), "%61+=-9.itch.zst");
    EXPECT_EQ(symbol_file("aapl"), "%61%61%70%6C.itch.zst");
    EXPECT_EQ(symbol_file("A%B"), "A%25B.itch.zst");
    EXPECT_EQ(symbol_file("A/B C"), "A%2FB%20C.itch.zst");
}

TEST(StoreIndexTest, RoundTripsThroughIndexTsvWithEscapedSymbols)
{
    const ScratchDir dir{"index-round-trip"};
    const StoreIndex index = two_days();
    EXPECT_EQ(index.text(),
              "day\t2019-12-27\t12272019.NASDAQ_ITCH50.bin\t1000\t0123456789abcdef0123456789abcdef\n"
              "day\t2019-12-30\t12302019.NASDAQ_ITCH50.bin\t2000\tbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
              "segment\t2019-12-27\tAAPL\t0\t40\t90\t7\t5\t0123456789abcdef0123456789abcdef\n"
              "segment\t2019-12-30\tA%25B\t0\t9\t12\t1\t0\t0123456789abcdef0123456789abcdef\n"
              "segment\t2019-12-30\tAAPL\t40\t30\t80\t6\t4\t0123456789abcdef0123456789abcdef\n"
              "segment\t2019-12-30\tBRK.B\t0\t20\t50\t3\t1\t0123456789abcdef0123456789abcdef\n");
    write_store(dir.path, index.text());
    StoreIndex loaded;
    std::string error;
    ASSERT_TRUE(loaded.load(dir.path, error)) << error;
    EXPECT_EQ(loaded, index);
    EXPECT_EQ(loaded.committed_end("AAPL"), 70u);
    EXPECT_EQ(loaded.committed_end("A%B"), 9u);
    EXPECT_EQ(loaded.committed_end("MSFT"), 0u);

    const std::filesystem::path empty = dir.path / "empty";
    std::filesystem::create_directory(empty);
    ASSERT_TRUE(loaded.load(empty, error)) << error;  // no index.tsv: an empty store
    EXPECT_TRUE(loaded.days().empty());
}

// A rebuilt day's records replace its old ones; its new segments, appended after the later day's, end each file.
TEST(StoreIndexTest, AReplacedDayDropsItsOldRecords)
{
    StoreIndex index = two_days();
    index.set(
        2019y / 12 / 27,
        {.file = "12272019.NASDAQ_ITCH50.bin",
         .size = 1'001,
         .hash = kHash,
         .segments = {
             {"AAPL", {.offset = 70, .compressed = 5, .uncompressed = 9, .messages = 1, .events = 1, .hash = kHash}},
             {"MSFT", {.offset = 0, .compressed = 7, .uncompressed = 9, .messages = 1, .events = 1, .hash = kHash}}}});
    ASSERT_EQ(index.days().size(), 2u);
    EXPECT_EQ(index.days().at(2019y / 12 / 27).size, 1'001u);
    EXPECT_EQ(index.days().at(2019y / 12 / 27).segments.at("AAPL").offset, 70u);
    EXPECT_EQ(index.committed_end("AAPL"), 75u);  // the 27th's new segment, after the 30th's
    EXPECT_EQ(index.committed_end("MSFT"), 7u);
    EXPECT_EQ(index.committed_end("BRK.B"), 20u);
}

// Each case breaks one rule of the index's format in an otherwise valid store; load names what it found.
TEST(StoreIndexTest, RefusesEveryInvalidIndex)
{
    const std::string valid = two_days().text();
    const auto replaced = [&](const std::string& from, const std::string& to) {
        std::string text = valid;
        text.replace(text.find(from), from.size(), to);
        return text;
    };
    const std::string aapl27 = "segment\t2019-12-27\tAAPL\t0\t40\t90\t7\t5\t0123456789abcdef0123456789abcdef\n";
    const std::vector<std::pair<std::string, std::string>> cases{
        {replaced("\t1000\t", "\t1000\t\t"), "line 1: not a record"},
        {replaced("\t7\t5\t", "\t7\t"), "line 3: not a record"},
        {replaced("day\t2019-12-27", "days\t2019-12-27"), "line 1: not a record"},
        {valid + "\n", "line 7: not a record"},
        {valid.substr(0, valid.size() - 1), "line 6: no end of line"},
        {replaced("2019-12-27\t1227", "2019-13-27\t1227"), "line 1: bad date"},
        {replaced("12272019.NASDAQ", "12262019.NASDAQ"), "line 1: bad file"},
        {replaced("\t1000\t", "\t+1000\t"), "line 1: bad number"},
        {replaced("\t40\t90\t", "\t40\t9x\t"), "line 3: bad number"},
        {replaced("\tAAPL\t40\t30\t", "\tAAPL\t40\t18446744073709551615\t"), "line 5: bad number"},  // overflows
        {replaced("\t1000\t", "\t18446744073709551616\t"), "line 1: bad number"},  // over 2^64-1
        {replaced("\t1000\t", "\t01000\t"), "line 1: bad number"},  // a second spelling
        {replaced("\t40\t90\t", "\t40\t\t"), "line 3: bad number"},
        {replaced("\t7\t5\t" + kHash, "\t7\t5\t" + kHash.substr(1)), "line 3: bad hash"},
        {replaced("\t1000\t" + kHash, "\t1000\t0123456789abcdef0123456789abcde"), "line 1: bad hash"},
        {replaced("\t1000\t" + kHash, "\t1000\t0123456789ABCDEF0123456789abcdef"), "line 1: bad hash"},
        {replaced("\tA%25B\t", "\tA%2fB\t"), "line 4: bad symbol"},
        {replaced("\tA%25B\t", "\tA%42\t"), "line 4: bad symbol"},
        {replaced("\tA%25B\t", "\tA%B\t"), "line 4: bad symbol"},
        {replaced("\tA%25B\t", "\tABCDEFGHI\t"), "line 4: bad symbol"},
        {replaced("\tA%25B\t", "\tA%20\t"), "line 4: bad symbol"},  // a trailing space is ITCH padding
        {replaced("day\t2019-12-30\t12302019", "day\t2019-12-27\t12272019"), "line 2: day repeated"},
        {valid + aapl27, "line 7: segment repeated"},
        {replaced("2019-12-27\tAAPL", "2019-12-31\tAAPL"), "line 3: segment without its day"},
        {replaced("\tBRK.B\t0\t20\t", "\tBRK.B\t1\t20\t"), "BRK.B.itch.zst: shorter than its segments"},
        {replaced("\tAAPL\t40\t30\t", "\tAAPL\t39\t30\t"), "AAPL.itch.zst: overlapping segments"},
    };
    for (const auto& [text, why] : cases) {
        const ScratchDir dir{"index-invalid"};
        write_store(dir.path, text);
        StoreIndex index;
        std::string error;
        EXPECT_FALSE(index.load(dir.path, error)) << why;
        EXPECT_EQ(error, why);
        EXPECT_TRUE(index.days().empty()) << why;  // never half-loaded
    }

    const ScratchDir moved{"index-moved"};  // a rebuilt day's segment lies before an older day's in the file
    std::string text = replaced("2019-12-27\tAAPL\t0\t40\t", "2019-12-27\tAAPL\t30\t40\t");
    const std::string later = "2019-12-30\tAAPL\t40\t30\t";
    text.replace(text.find(later), later.size(), "2019-12-30\tAAPL\t0\t30\t");
    write_store(moved.path, text);
    StoreIndex reordered;
    std::string reason;
    EXPECT_TRUE(reordered.load(moved.path, reason)) << reason;

    const ScratchDir dir{"index-files"};
    write_store(dir.path, valid, 10);
    std::filesystem::remove(dir.path / "BRK.B.itch.zst");
    StoreIndex index;
    std::string error;
    EXPECT_FALSE(index.load(dir.path, error));
    EXPECT_EQ(error, "BRK.B.itch.zst: No such file or directory");
    std::filesystem::create_directory(dir.path / "BRK.B.itch.zst");
    EXPECT_FALSE(index.load(dir.path, error));
    EXPECT_EQ(error, "BRK.B.itch.zst: not a file");
    std::filesystem::remove(dir.path / "index.tsv");
    std::filesystem::create_directory(dir.path / "index.tsv");
    EXPECT_FALSE(index.load(dir.path, error));
    EXPECT_EQ(error, "index.tsv: not a file");

    if (::geteuid() != 0) {  // root reads any file
        const ScratchDir unreadable{"index-unreadable"};
        write_store(unreadable.path, valid);
        std::filesystem::permissions(unreadable.path / "index.tsv", std::filesystem::perms::none);
        EXPECT_FALSE(index.load(unreadable.path, error));
        EXPECT_EQ(error, "index.tsv: cannot be read");
    }
}

}  // namespace
}  // namespace simulator::store_test
