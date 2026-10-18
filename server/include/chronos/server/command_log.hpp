#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <span>
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

// Reads the log at `path`, which must have exactly one header for `generation`, and refuses any line that does not
// parse back to exactly the text the writer wrote. On success `start` is the header's starting strategy and
// `generation`'s Applied records are appended to `applied`; otherwise false with `error` naming the line.
bool read_log(const std::filesystem::path& path, std::uint64_t generation, StrategySpec& start,
              std::vector<Record>& applied, std::string& error);

// Attaches `start` to a fresh `generation`, replays `applied` at their boundaries, then to the end; false on mismatch.
bool reproduce(Generation& generation, const StrategySpec& start, std::span<const Record> applied) noexcept;

}  // namespace simulator::server
