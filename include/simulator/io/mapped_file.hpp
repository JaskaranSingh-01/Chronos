#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace simulator {

// A whole file mapped read-only (POSIX mmap, MAP_PRIVATE, MADV_SEQUENTIAL).
// Move-only; the span from bytes() stays valid while this object owns the mapping.
class MappedFile
{
private:
    const std::byte* data_{nullptr};
    std::size_t size_{0};
    std::array<char, 256> error_{};  // "" or e.g. "open /x/y.bin: No such file or directory"

    void reset() noexcept;

public:
    MappedFile() noexcept = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    // Unmaps any previous file first. An empty file maps to an empty span.
    // On false, bytes() is empty and error() says which call failed and why.
    bool open(const char* path) noexcept;

    std::span<const std::byte> bytes() const noexcept
    {
        return {data_, size_};
    }

    const char* error() const noexcept
    {
        return error_.data();
    }
};

} // namespace simulator
