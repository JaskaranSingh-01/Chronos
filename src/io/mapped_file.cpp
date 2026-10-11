#include "simulator/io/mapped_file.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace simulator {

void MappedFile::reset() noexcept
{
    if (data_ != nullptr) {
        ::munmap(const_cast<std::byte*>(data_), size_);
    }
    data_ = nullptr;
    size_ = 0;
    error_[0] = '\0';
}

MappedFile::~MappedFile()
{
    reset();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      error_(other.error_)
{
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept
{
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        error_ = other.error_;
    }
    return *this;
}

bool MappedFile::open(const char* path) noexcept
{
    reset();

    const auto fail = [&](const char* call) {
        std::snprintf(error_.data(), error_.size(), "%s %s: %s", call, path, std::strerror(errno));
        return false;
    };

    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return fail("open");
    }

    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        fail("fstat");
        ::close(fd);
        return false;
    }

    // mmap of length 0 fails, so an empty file stays an empty span.
    const auto size = static_cast<std::size_t>(status.st_size);
    if (size == 0) {
        ::close(fd);
        return true;
    }

    void* data = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        fail("mmap");
        ::close(fd);
        return false;
    }
    ::close(fd);  // the mapping keeps the file alive

    ::madvise(data, size, MADV_SEQUENTIAL);  // advisory; failure is harmless
    data_ = static_cast<const std::byte*>(data);
    size_ = size;
    return true;
}

} // namespace simulator
