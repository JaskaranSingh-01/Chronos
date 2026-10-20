#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chronos/server/backtest.hpp"

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
    std::optional<RunState> verifying{};         // a verification Queued or Running; not saved
    std::vector<Verification> verifications{};
    double progress{0};  // the bytes read over the days' sizes while running; not saved
};

}  // namespace simulator::server
