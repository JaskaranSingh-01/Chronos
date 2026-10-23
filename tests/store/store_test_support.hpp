#pragma once

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace simulator::store_test {

// A directory in the system temp directory, unique to this process; removed with the object.
struct ScratchDir
{
    std::filesystem::path path;

    explicit ScratchDir(const std::string& name)
        : path(std::filesystem::temp_directory_path() / ("simulator_tests-" + std::to_string(::getpid()) + "-" + name))
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }

    ~ScratchDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

inline void write_file(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream{path, std::ios::binary} << text;
}

inline std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

}  // namespace simulator::store_test
