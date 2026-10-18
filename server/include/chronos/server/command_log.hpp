#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "chronos/server/channels.hpp"
#include "chronos/server/generation.hpp"
#include "chronos/server/replay_session.hpp"

namespace simulator::server {

// The command log's format version, written in every header line.
inline constexpr int log_protocol = 1;

// The log writer's queue: a push never waits, and once the queue has been full or the writer failed, it stays failed.
class LogQueue
{
public:
    static constexpr std::size_t capacity = 4'096;

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    FixedQueue<Record> records_{capacity};
    std::atomic<std::size_t> depth_{0};  // records_.size(), set under mutex_, so the engine reads it without the lock
    std::size_t reserved_{0};
    bool closed_{false};
    std::atomic<bool> failed_{false};

public:
    // False, failing the log, if it has failed or the queue is full.
    bool push(const Record& record) noexcept;
    // Holds a slot for push_reserved(); false, failing the log, as push() would be.
    bool reserve() noexcept;
    // Pushes into the slot reserve() held; never fails.
    void push_reserved(const Record& record) noexcept;
    // Waits for a record; false once closed and drained.
    bool pop(Record& record);
    // Lets the writer drain what is queued and stop.
    void close();
    void fail() noexcept;
    bool failed() const noexcept;
    std::size_t size() const noexcept;
    // True while a healthy log holds more than half its capacity: the engine holds market work for the writer.
    bool backed_up() const noexcept
    {
        return depth_.load(std::memory_order_relaxed) > capacity / 2 && !failed_.load(std::memory_order_relaxed);
    }
};

// Appends records to `path` as JSON lines on its own thread, flushing each.
// With `fsync`, each command line is synced to the device, and so is the directory when the file is created.
class LogWriter
{
private:
    LogQueue& queue_;
    SessionConfig config_;
    std::FILE* file_;
    bool fsync_;
    std::atomic<std::uint64_t> persisted_seq_{0};
    std::thread thread_;

    void run();

public:
    LogWriter(LogQueue& queue, const std::filesystem::path& path, SessionConfig config, bool fsync);
    ~LogWriter();

    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    // The seq of the last record written and flushed (and synced, for a command line, with fsync on).
    std::uint64_t persisted_seq() const noexcept;
};

// One generation of a command log: what reproduce() needs.
struct LoggedGeneration
{
    Ticker ticker{};  // its input
    StrategySpec start{};  // the strategy it started with
    std::vector<Record> applied{};  // its Applied command steps, in log order
    std::optional<Record> end{};  // its trailer, if the session closed it: where it stopped, and its digests
};

// Reads the log at `path`, which must have exactly one header for `generation`, and refuses any line that does not
// parse back to exactly the text the writer wrote. False, with `error` naming the line, if it cannot.
bool read_log(const std::filesystem::path& path, std::uint64_t generation, LoggedGeneration& logged,
              std::string& error);

// Attaches the logged start strategy to the fresh `generation` (built over the logged ticker), replays the applied
// commands at their boundaries, then on to the trailer's boundary, or to the end without one; false on a mismatch,
// including the trailer's digests.
bool reproduce(Generation& generation, const LoggedGeneration& logged) noexcept;

}  // namespace simulator::server
