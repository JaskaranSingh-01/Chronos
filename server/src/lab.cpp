#include "chronos/server/lab.hpp"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <new>
#include <system_error>
#include <utility>

#include "build_info.hpp"  // generated: the build's commit, source digest, compiler and the rest
#include "chronos/server/protocol.hpp"
#include "chronos/server/security.hpp"

namespace simulator::server {
namespace {

// Now in UTC, e.g. 2026-10-18T10:15:00Z.
std::string utc_now()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    char text[24];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

std::string host_name()
{
    char name[256] = {};
    return ::gethostname(name, sizeof name - 1) == 0 ? name : "";
}

// Writes `text` to `path` through a temp file and a rename; the error, or "" once it is in place.
std::string write_file(const std::filesystem::path& path, const std::string& text)
{
    const std::filesystem::path temp = path.string() + ".tmp";
    std::error_code error;
    std::error_code ignored;  // a cleanup's own, so it never clears `error`
    {
        std::ofstream out{temp, std::ios::binary};
        out << text;
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ignored);
            return "cannot write " + temp.string();
        }
    }
    std::filesystem::rename(temp, path, error);
    if (error) {
        std::filesystem::remove(temp, ignored);
        return error.message();
    }
    return {};
}

// A verification's record (spec §5.5): the days whose re-hash differs from the run's, then this build's source digest
// against the run's, then the digests.
Verification verification(const Run& run, const RunResult& result)
{
    Verification v{.finished = utc_now(), .source_digest = build_source_digest};
    for (std::size_t i = 0; i < run.provenance.days.size(); ++i) {
        if (i < result.hashes.size() && result.hashes[i] != run.provenance.days[i].hash) {
            v.changed.push_back(run.provenance.days[i].date);
        }
    }
    if (!result.partial) {
        v.digests = result.digests;
    }
    v.outcome = verdict(!v.changed.empty(), run.provenance.build.source_digest == build_source_digest,
                        v.digests && run.results != nullptr && *v.digests == run.results->digests);
    return v;
}

}  // namespace

Backtester::Backtester(SessionConfig config, std::filesystem::path dir)
    : config_(std::move(config)), dir_(std::move(dir))
{
    std::error_code error;
    std::filesystem::create_directories(dir_, error);
    if (error) {
        std::fprintf(stderr, "warning: cannot create %s: %s; runs stay in memory\n", dir_.c_str(),
                     error.message().c_str());
    }
    for (std::filesystem::directory_iterator files{dir_, error};
         !error && files != std::filesystem::directory_iterator{}; files.increment(error)) {
        const std::filesystem::path& path = files->path();
        if (path.extension() != ".json" || path.filename() == "fingerprints.json") {  // the day library's cache
            continue;
        }
        std::ifstream in{path, std::ios::binary};
        const std::string text{std::istreambuf_iterator<char>{in}, {}};
        Run run;
        if (!decode_run(text, run) || run.id != path.stem().string()) {
            std::fprintf(stderr, "skipping %s: not a saved run\n", path.c_str());
            continue;
        }
        run.progress = 1;
        std::string id = run.id;
        runs_.emplace(std::move(id), Entry{.run = std::move(run)});
    }
    worker_ = std::thread{[this] { work(); }};
}

Backtester::~Backtester()
{
    {
        const std::lock_guard lock{mutex_};
        stop_ = true;
        cancel_.store(true);
    }
    wake_.notify_all();
    worker_.join();
}

std::filesystem::path Backtester::file(std::string_view id) const
{
    return dir_ / (std::string{id} + ".json");
}

Refusal Backtester::submit(const RunSpec& spec, std::vector<DayHandle> days, std::string& id)
{
    Run run{.id = new_session_id(),
            .spec = spec,
            .provenance = {.build = {.commit = build_commit,
                                     .dirty = build_dirty,
                                     .source_digest = build_source_digest,
                                     .compiler = build_compiler,
                                     .build_type = build_type,
                                     .sanitize = build_sanitize,
                                     .platform = build_platform},
                           .book_capacity = config_.book_capacity,
                           .price_scale = config_.instrument.price_scale,
                           .quantity_scale = config_.instrument.quantity_scale,
                           .check_invariants_every = config_.check_invariants_every,
                           .allow_truncated_tail = config_.allow_truncated_tail,
                           .host = host_name(),
                           .submitted = utc_now()}};
    for (const DayHandle& day : days) {
        run.provenance.days.push_back(
            {.date = day->date, .file = day->file, .size = day->size, .hash = day->hash, .session = day->session});
    }
    {
        const std::lock_guard lock{mutex_};
        if (queue_.size() >= max_queued) {
            return Refusal::QueueFull;
        }
        id = run.id;
        queue_.push_back({.id = id, .days = std::move(days)});
        runs_.emplace(id, Entry{.run = std::move(run)});
    }
    wake_.notify_one();
    return Refusal::None;
}

Refusal Backtester::cancel(std::string_view id)
{
    Run cancelled;
    {
        const std::lock_guard lock{mutex_};
        const auto found = runs_.find(id);
        if (found == runs_.end() || found->second.deleted) {
            return Refusal::NotFound;
        }
        Entry& entry = found->second;
        if (entry.run.state == RunState::Running) {
            cancel_.store(true);  // seen at the job's next market event
            return Refusal::None;
        }
        if (entry.run.state != RunState::Queued) {
            return Refusal::Conflict;
        }
        std::erase_if(queue_, [&](const Job& job) { return job.id == id; });
        entry.run.state = RunState::Cancelled;
        entry.run.progress = 1;
        entry.run.provenance.finished = utc_now();
        entry.run.results = std::make_shared<const RunResult>(RunResult{.partial = true, .cancelled = true});
        entry.busy = true;
        cancelled = entry.run;
    }
    save(std::move(cancelled));
    return Refusal::None;
}

Refusal Backtester::verify(std::string_view id, const DayLibrary& library)
{
    std::vector<RunDay> recorded;
    {
        const std::lock_guard lock{mutex_};
        const auto found = runs_.find(id);
        if (found == runs_.end() || found->second.deleted) {
            return Refusal::NotFound;
        }
        const Run& run = found->second.run;
        if (run.verifying) {
            return Refusal::None;  // the one queued or running
        }
        if (run.state != RunState::Completed || run.results == nullptr || run.results->partial) {
            return Refusal::Conflict;
        }
        recorded = run.provenance.days;
    }
    std::vector<DayHandle> days;
    for (const RunDay& day : recorded) {
        DayHandle listed = library.find(day.date);
        if (listed == nullptr || listed->hash != day.hash) {
            return Refusal::InputChanged;
        }
        days.push_back(std::move(listed));
    }
    {
        const std::lock_guard lock{mutex_};
        const auto found = runs_.find(id);
        if (found == runs_.end() || found->second.deleted) {
            return Refusal::NotFound;
        }
        if (found->second.run.verifying) {
            return Refusal::None;
        }
        if (queue_.size() >= max_queued) {
            return Refusal::QueueFull;
        }
        found->second.run.verifying = RunState::Queued;
        queue_.push_back({.id = std::string{id}, .days = std::move(days), .verify = true});
    }
    wake_.notify_one();
    return Refusal::None;
}

bool Backtester::watch(Control& control, const DayLibrary& library, std::string& why) const
{
    const std::string_view id = name(control.watch.run_id);
    Run run;
    {
        const std::lock_guard lock{mutex_};
        const auto found = runs_.find(id);
        if (found == runs_.end() || found->second.deleted) {
            why = std::string{id} + ": no such run";
            return false;
        }
        run = found->second.run;
    }
    if (run.state != RunState::Completed || run.results == nullptr || run.results->partial) {
        why = std::string{id} + ": not a completed run";
        return false;
    }
    if (run.provenance.allow_truncated_tail != config_.allow_truncated_tail) {
        why = std::string{id} + ": run with another --allow-truncated-tail";
        return false;
    }
    std::vector<std::string> hashes;
    for (const RunDay& day : run.provenance.days) {
        const DayHandle listed = library.find(day.date);
        if (listed == nullptr || listed->hash != day.hash) {
            why = iso_date(day.date) + ": not the day the run replayed";
            return false;
        }
        hashes.push_back(day.hash);
    }
    control.spec = run.spec;
    control.watch.expected = run.results->digests;
    const std::string inputs = inputs_hash(hashes);
    std::copy(inputs.begin(), inputs.end(), control.watch.inputs.begin());
    control.watch.same_code = run.provenance.build.source_digest == build_source_digest;
    return true;
}

Refusal Backtester::remove(std::string_view id, std::string& warning)
{
    {
        const std::lock_guard lock{mutex_};
        const auto found = runs_.find(id);
        if (found == runs_.end() || found->second.deleted) {
            return Refusal::NotFound;
        }
        Entry& entry = found->second;
        if (entry.busy) {
            entry.deleted = true;
            if (running_ == id) {
                cancel_.store(true);
            }
            return Refusal::None;
        }
        const bool saved = entry.run.state != RunState::Queued;
        std::erase_if(queue_, [&](const Job& job) { return job.id == id; });
        runs_.erase(found);
        if (!saved) {
            return Refusal::None;
        }
    }
    std::error_code error;
    std::filesystem::remove(file(id), error);
    if (error) {
        warning = "not removed: " + error.message();
    }
    return Refusal::None;
}

std::string Backtester::list() const
{
    std::vector<Run> runs;
    std::size_t queued = 0;
    {
        const std::lock_guard lock{mutex_};
        for (const auto& [id, entry] : runs_) {
            if (entry.deleted) {
                continue;
            }
            Run& run = runs.emplace_back(entry.run);
            if (id == running_ && run.state == RunState::Running && running_size_ != 0) {
                run.progress = static_cast<double>(bytes_read_.load()) / static_cast<double>(running_size_);
            }
        }
        queued = queue_.size();
    }
    return encode_runs(runs, queued);
}

Refusal Backtester::get(std::string_view id, std::string& json) const
{
    Run run;
    {
        const std::lock_guard lock{mutex_};
        const auto found = runs_.find(id);
        if (found == runs_.end() || found->second.deleted) {
            return Refusal::NotFound;
        }
        run = found->second.run;
    }
    json = encode_run(run);
    return Refusal::None;
}

void Backtester::work()
{
    for (;;) {
        Job job;
        RunSpec spec;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            Entry& entry = runs_.at(job.id);
            entry.busy = true;
            if (job.verify) {
                entry.run.verifying = RunState::Running;
            } else {
                entry.run.state = RunState::Running;
                entry.run.provenance.started = utc_now();
            }
            spec = entry.run.spec;
            running_ = job.id;
            running_size_ = 0;
            for (const DayHandle& day : job.days) {
                running_size_ += day->size;
            }
            cancel_.store(false);
            bytes_read_.store(0);
        }
        std::vector<DayHandle> days = job.days;
        if (job.verify) {
            std::transform(days.begin(), days.end(), days.begin(), [](const DayHandle& day) { return reopen(*day); });
        }
        RunResult result;
        try {
            if (std::find(days.begin(), days.end(), nullptr) == days.end()) {
                result = backtest(config_, spec, days, job.verify, cancel_, bytes_read_);
            } else {  // a file gone since the verification was queued: no replay, and each day's re-hash
                result.partial = true;
                for (const DayHandle& day : days) {
                    result.hashes.push_back(day != nullptr ? file_hash(day->path) : "");
                }
            }
        } catch (const std::bad_alloc&) {
            result = RunResult{.partial = true, .reason = "out_of_memory"};
        } catch (const std::exception& e) {
            result = RunResult{.partial = true, .reason = e.what()};
        } catch (...) {
            result = RunResult{.partial = true, .reason = "unknown_error"};
        }
        finish(job, std::move(result));
    }
}

// The job has stopped: a deleted run is erased, its file with it, and a shutdown keeps nothing; else the run takes its
// results and is saved.
void Backtester::finish(const Job& job, RunResult result)
{
    Run run;
    {
        const std::lock_guard lock{mutex_};
        running_.clear();
        const auto found = runs_.find(job.id);
        if (stop_) {
            return;
        }
        if (found->second.deleted) {
            runs_.erase(found);
        } else {
            found->second.run.progress = 1;
            run = found->second.run;
        }
    }
    if (run.id.empty()) {
        std::error_code ignored;
        std::filesystem::remove(file(job.id), ignored);
        return;
    }
    if (job.verify) {
        run.verifying.reset();
        run.verifications.push_back(verification(run, result));
        save(std::move(run));
        return;
    }
    run.state = result.cancelled ? RunState::Cancelled : result.partial ? RunState::Failed : RunState::Completed;
    run.reason = result.reason;
    run.provenance.finished = utc_now();
    run.provenance.wall_ms = result.wall_ms;
    run.provenance.cpu_ms = result.cpu_ms;
    run.results = std::make_shared<const RunResult>(std::move(result));
    save(std::move(run));
}

// Writes the busy run `run` to its file, then publishes it and lets it go; a run deleted meanwhile is erased, its file
// with it.
void Backtester::save(Run run)
{
    const std::string id = run.id;
    std::string error;
    try {
        error = write_file(file(id), encode_run(run));
    } catch (...) {  // the worker's thread must not end on an exception (spec §9)
        error = "cannot encode the run";
    }
    if (!error.empty()) {
        run.warning = "not saved: " + error;
    }
    {
        const std::lock_guard lock{mutex_};
        const auto found = runs_.find(id);
        if (!found->second.deleted) {
            found->second.run = std::move(run);
            found->second.busy = false;
            return;
        }
        runs_.erase(found);
    }
    std::error_code ignored;
    std::filesystem::remove(file(id), ignored);
}

}  // namespace simulator::server
