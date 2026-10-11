#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "simulator/io/mapped_file.hpp"

namespace simulator {
namespace {

const std::string basic_replay = std::string{CHRONOS_TEST_DATA_DIR} + "/basic_replay.csv";

std::string read_file(const std::string& path)
{
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::string as_string(std::span<const std::byte> bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

TEST(MappedFileTest, MapsAFileByteForByte)
{
    MappedFile file;

    ASSERT_TRUE(file.open(basic_replay.c_str())) << file.error();
    EXPECT_STREQ(file.error(), "");

    const std::string expected = read_file(basic_replay);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(as_string(file.bytes()), expected);
}

TEST(MappedFileTest, MissingFileGivesFalseAndAnError)
{
    MappedFile file;

    EXPECT_FALSE(file.open("/nonexistent/chronos/missing.bin"));
    EXPECT_TRUE(file.bytes().empty());
    EXPECT_NE(std::string{file.error()}.find("missing.bin"), std::string::npos) << file.error();
}

TEST(MappedFileTest, EmptyFileGivesAnEmptySpan)
{
    const auto path = std::filesystem::temp_directory_path() / "chronos_mapped_file_empty.bin";
    std::ofstream{path}.close();

    MappedFile file;
    const bool opened = file.open(path.c_str());
    std::filesystem::remove(path);

    EXPECT_TRUE(opened) << file.error();
    EXPECT_TRUE(file.bytes().empty());
}

TEST(MappedFileTest, MoveTransfersTheMapping)
{
    MappedFile first;
    ASSERT_TRUE(first.open(basic_replay.c_str()));
    const std::string expected = as_string(first.bytes());

    MappedFile second{std::move(first)};
    EXPECT_TRUE(first.bytes().empty());
    EXPECT_EQ(as_string(second.bytes()), expected);

    MappedFile third;
    third = std::move(second);
    EXPECT_TRUE(second.bytes().empty());
    EXPECT_EQ(as_string(third.bytes()), expected);
}

} // namespace
} // namespace simulator
