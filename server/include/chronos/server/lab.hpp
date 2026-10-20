#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "chronos/server/backtest.hpp"
#include "chronos/server/replay_session.hpp"

namespace simulator::server {

// ponytail: one worker and 8 waiting jobs (runs or verifications); a pool of workers, each with its Generation.
inline constexpr std::size_t max_queued = 8;

enum class RunState : std::uint8_t {
    Queued,
    Running,
    Completed,
    Failed,  // with a reason
    Cancelled
};

// The build a run ran on: its source digest is part of the run's identity, the rest are labels.
struct BuildInfo
{
    std::string commit{};
    bool dirty{false};
    std::string source_digest{};
    std::string compiler{};
    std::string build_type{};
    std::string sanitize{};
    std::string platform{};
};

// A day a run replayed, as the library listed it when the run was submitted.
struct RunDay
{
    std::chrono::year_month_day date{};
    std::string file{};
    std::uint64_t size{0};
    std::string hash{};
    FeedSession session{};
};

// What a run's results depend on, and where and when it ran (spec §7.3, §8).
struct Provenance
{
    BuildInfo build{};
    std::size_t book_capacity{0};
    std::uint8_t price_scale{0};
    std::uint8_t quantity_scale{0};
    std::uint64_t check_invariants_every{0};
    bool allow_truncated_tail{false};
    std::vector<RunDay> days{};
    std::string host{};
    std::string submitted{};  // UTC, e.g. 2026-10-18T10:15:00Z; empty until it happens
    std::string started{};
    std::string finished{};
    std::int64_t wall_ms{0};
    std::int64_t cpu_ms{0};
};

struct Verification
{
    std::string finished{};
    Outcome outcome{Outcome::Mismatch};
    std::optional<Digests> digests{};  // none when the re-run failed
    std::string source_digest{};       // of the build that verified
    std::vector<std::chrono::year_month_day> changed{};  // input_changed: the days whose re-hash differs
};

// One Lab run: runs/<id>.json, and GET /api/backtests/<id>.
struct Run
{
    std::string id{};
    RunState state{RunState::Queued};
    std::string reason{};   // failed: why
    std::string warning{};  // e.g. "not saved: No space left on device"
    RunSpec spec{};
    Provenance provenance{};
    std::shared_ptr<const RunResult> results{};  // null while queued or running
    std::optional<RunState> verifying{};  // a verification Queued or Running; written, but decode_run drops it
    std::vector<Verification> verifications{};
    double progress{0};  // the bytes read over the days' sizes while running; not saved
};

// Why the Backtester refused a request.
enum class Refusal : std::uint8_t {
    None,
    NotFound,  // no such run, or one being deleted
    Conflict,      // the run is not in a state that allows it
    InputChanged,  // a day of the run is gone, or its cached hash is not the run's
    QueueFull      // max_queued jobs wait already
};

// The Lab's runs (spec §5.3-§5.5) and the one worker thread that runs their jobs, runs and verifications, oldest
// first. The worker writes a run's file, <dir>/<id>.json, through a temp file and a rename as the run finishes, and
// again after each verification; never earlier. A run deleted while a job of it is busy is hidden at once, and
// erased, its file with it, when the worker stops the job. The mutex covers the runs and the queue for a lookup or a
// state change only: no file, hash or replay work runs under it.
class Backtester
{
private:
    struct Job
    {
        std::string id;
        std::vector<DayHandle> days;  // as the library listed them when the job was queued
        bool verify{false};  // a verification of the completed run `id`
    };

    struct Entry
    {
        Run run;
        bool busy{false};  // its job is running, or its file is being written
        bool deleted{false};  // a tombstone: hidden, and erased once the busy job stops
    };

    SessionConfig config_;
    std::filesystem::path dir_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    // ponytail: every run's results stay in memory, ~3 MB of fills plus ~1 MB of equity a day, up to 32 days a run;
    // read them from their files past a few hundred runs.
    std::map<std::string, Entry, std::less<>> runs_;
    std::deque<Job> queue_;
    std::string running_;  // the id of the run whose job is running; "" if none
    std::uint64_t running_size_{0};  // its days' sizes
    std::atomic<bool> cancel_{false};  // the running job's
    std::atomic<std::uint64_t> bytes_read_{0};
    bool stop_{false};
    std::thread worker_;

    void work();
    void finish(const Job& job, RunResult result);
    void save(Run run);
    std::filesystem::path file(std::string_view id) const;

public:
    // Loads every run saved in `dir`, creating it if it is missing, and starts the worker.
    Backtester(SessionConfig config, std::filesystem::path dir);
    // Cancels the running job and drops the queued ones; neither is saved.
    ~Backtester();

    Backtester(const Backtester&) = delete;
    Backtester& operator=(const Backtester&) = delete;

    // Queues a run of `spec` over `days`, the library's handles of spec.days, and names it `id`.
    Refusal submit(const RunSpec& spec, std::vector<DayHandle> days, std::string& id);
    // Cancels a queued run at once, and a running one at its next market event; Conflict once it has ended.
    Refusal cancel(std::string_view id);
    // Queues a verification of a completed run over its days as `library` lists them now, or keeps the one already
    // queued or running: a backtest() of its spec with hash as read, over fresh mappings. Conflict for a run that is
    // not completed or partial; InputChanged if a day's cached hash is not the run's.
    Refusal verify(std::string_view id, const DayLibrary& library);
    // A Watch in Terminal's admission (spec §5.5): fills `control`'s spec and watch from the completed run
    // control.watch.run_id, if each of its days is listed with the run's hash and it ran with this server's
    // allow_truncated_tail. False, with `why`, if not.
    bool watch(Control& control, const DayLibrary& library, std::string& why) const;
    // Erases a queued or idle run, and an idle run's file; a busy one is a tombstone until its job stops. `warning`
    // says why a file could not be removed.
    Refusal remove(std::string_view id, std::string& warning);
    // GET /api/backtests.
    std::string list() const;
    // GET /api/backtests/<id>.
    Refusal get(std::string_view id, std::string& json) const;
};

}  // namespace simulator::server
