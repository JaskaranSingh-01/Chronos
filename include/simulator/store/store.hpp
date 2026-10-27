#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "simulator/store/store_index.hpp"

namespace simulator {

// When a build writes its buffers: a symbol's alone once it holds `flush` bytes, and, before they would hold more than
// `cap` bytes allocated together, the largest first until they hold under `low`. So no more than `cap` bytes are ever
// allocated to buffers.
struct StoreLimits
{
    std::size_t flush{std::size_t{4} << 20};
    std::size_t cap{std::size_t{512} << 20};
    std::size_t low{std::size_t{256} << 20};
};

// Builds one day into the store at `store` from the day file `source`, fed its frames in file order.
// A symbol's segment holds every frame of its stock locate from its directory entry on, and every frame that reaches
// every symbol: each System Event, each frame no locate routes, and a cut tail; each in file order, as zstd frames
// appended to its file. Destroyed uncommitted, it aborts: each file it wrote is cut back to its committed end.
class StoreBuilder
{
private:
    struct Build;
    std::unique_ptr<Build> build_;

public:
    StoreBuilder(std::filesystem::path store, std::filesystem::path source, std::chrono::year_month_day date,
                 StoreLimits limits = {});
    ~StoreBuilder();

    StoreBuilder(const StoreBuilder&) = delete;
    StoreBuilder& operator=(const StoreBuilder&) = delete;

    // The day's next frame, its u16 length and its message; a last one cut short ends every segment. The first error
    // stops the build, and commit() reports it.
    void add(std::span<const std::byte> frame);
    // Writes the rest and renames the index with the day's record, `hash` its file's as read, over index.tsv: the rename
    // is the commit, and from then on this is true. False, with `error`, after an abort (a failed write or check).
    bool commit(std::string_view hash, std::string& error);
    // The index in force once committed.
    const StoreIndex& index() const noexcept;
    // The most bytes allocated to buffers at once.
    std::size_t peak_buffered() const noexcept;
};

// Reads `symbol`'s segment on `date`, as `segment` describes it, from its file in `store` into `bytes`: plain reads,
// then its range within the file, the length its zstd frames state (before anything is allocated to it), each frame's
// checksum and its hash are checked. False, with `error` naming the symbol and day, if any is not as described.
bool read_segment(const std::filesystem::path& store, std::string_view symbol, std::chrono::year_month_day date,
                  const Segment& segment, std::vector<std::byte>& bytes, std::string& error);

// ponytail: a rebuilt day's old segments stay in their files, about a third of its day file each rebuild; compact
// (rewrite each file without them, under the store's lock) if that space matters.
struct StoreUsage
{
    std::uint64_t bytes{0};  // the symbol files'
    std::uint64_t unreferenced{0};  // those no segment of the index holds: rebuilt days' and interrupted builds'
};

StoreUsage store_usage(const std::filesystem::path& store, const StoreIndex& index);

}  // namespace simulator
