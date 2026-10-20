#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "chronos/server/itch_input.hpp"
#include "chronos/server/lab.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using namespace std::chrono_literals;
using nlohmann::json;

// A day long enough that a run of it is still running while a test queues, cancels or deletes others.
constexpr std::size_t kLongDay = 200'000;

// Two days in a scratch directory, a short Friday and a long Monday, and the runs directory beside them.
struct Days
{
    ScratchDir dir;
    DayLibrary library{dir.path / "data", dir.path / "fingerprints.json"};
    std::filesystem::path runs{dir.path / "runs"};

    explicit Days(const std::string& name)
        : dir(name)
    {
        std::filesystem::create_directories(dir.path / "data");
        write_file(dir.path / "data" / "12272019.NASDAQ_ITCH50.bin", lab_day(1).bytes());
        write_file(dir.path / "data" / "12302019.NASDAQ_ITCH50.bin", lab_day(2, kLongDay).bytes());
        library.scan();
    }

    // Submits the market maker on ACME over Friday, or over the long Monday.
    std::string submit(Backtester& lab, bool long_day = false, Refusal expected = Refusal::None)
    {
        RunSpec spec = default_spec(SessionStrategyKind::MarketMaker);
        spec.ticker = ticker("ACME");
        spec.days = {.day = {long_day ? 2019y / 12 / 30 : 2019y / 12 / 27}, .count = 1};
        std::string id;
        EXPECT_EQ(lab.submit(spec, {library.find(spec.days.day[0])}, id), expected);
        return id;
    }
};

SessionConfig lab_config()
{
    return SessionConfig{.session_id = "test", .instrument = kEquity};
}

// The run `id` as GET /api/backtests/<id> serves it; null if there is none.
json run(const Backtester& lab, const std::string& id)
{
    std::string text;
    return lab.get(id, text) == Refusal::None ? json::parse(text) : json(nullptr);
}

// The progress GET /api/backtests lists for the run `id`.
double progress(const Backtester& lab, const std::string& id)
{
    const json list = json::parse(lab.list());
    for (const json& listed : list["runs"]) {
        if (listed["id"] == id) {
            return listed["progress"].get<double>();
        }
    }
    return -1;
}

// Each run's state in one listing, in the order of `ids`; "gone" for a run not listed.
std::vector<std::string> states(const Backtester& lab, const std::vector<std::string>& ids)
{
    const json list = json::parse(lab.list());
    std::vector<std::string> found;
    for (const std::string& id : ids) {
        const auto listed =
            std::find_if(list["runs"].begin(), list["runs"].end(), [&](const json& run) { return run["id"] == id; });
        found.push_back(listed == list["runs"].end() ? "gone" : (*listed)["state"].get<std::string>());
    }
    return found;
}

std::string state(const Backtester& lab, const std::string& id)
{
    const json j = run(lab, id);
    return j.is_null() ? "gone" : j["state"].get<std::string>();
}

// Waits until the run `id` has finished or is gone.
bool finished(const Backtester& lab, const std::string& id)
{
    return eventually([&] {
        const std::string s = state(lab, id);
        return s == "completed" || s == "failed" || s == "cancelled" || s == "gone";
    });
}

std::string read(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

// One worker runs the jobs in the order they came; a ninth waiting job is refused.
TEST(LabTest, RunsItsJobsInOrderAndRefusesANinthWaitingOne)
{
    Days days{"lab-fifo"};
    Backtester lab{lab_config(), days.runs};
    std::vector<std::string> ids{days.submit(lab, true)};
    ASSERT_TRUE(eventually([&] { return state(lab, ids[0]) == "running"; }));
    for (std::size_t i = 0; i < max_queued; ++i) {
        ids.push_back(days.submit(lab));
    }
    days.submit(lab, false, Refusal::QueueFull);
    EXPECT_EQ(json::parse(lab.list())["queued"], max_queued);

    // In every listing the runs read completed, then at most one running, then queued: while one runs, every run before
    // it has finished and every one after it waits; between two jobs, none runs.
    bool ordered = true;
    ASSERT_TRUE(eventually([&] {
        const std::vector<std::string> now = states(lab, ids);
        const auto rank = [](const std::string& s) { return s == "completed" ? 0 : s == "running" ? 1 : 2; };
        ordered = ordered && std::count(now.begin(), now.end(), "running") <= 1 &&
            std::is_sorted(now.begin(), now.end(),
                           [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
        return std::count(now.begin(), now.end(), "completed") == 9;
    }));
    EXPECT_TRUE(ordered);
    for (std::size_t i = 2; i < ids.size(); ++i) {  // the same run, wherever it waited in the queue
        EXPECT_EQ(run(lab, ids[i])["results"]["digests"], run(lab, ids[1])["results"]["digests"]);
    }
}

// A queued run is cancelled at once; a running one stops at its next market event and keeps what it had. An ended run
// cannot be cancelled again.
TEST(LabTest, CancelsAQueuedOrRunningRunButNotAnEndedOne)
{
    Days days{"lab-cancel"};
    Backtester lab{lab_config(), days.runs};
    const std::string first = days.submit(lab, true);
    const std::string running = days.submit(lab, true);
    const std::string queued = days.submit(lab);
    ASSERT_TRUE(eventually([&] { return progress(lab, first) > 0; }));

    EXPECT_EQ(lab.cancel(queued), Refusal::None);
    EXPECT_EQ(state(lab, queued), "cancelled");
    EXPECT_EQ(run(lab, queued)["results"]["partial"], true);
    EXPECT_TRUE(std::filesystem::exists(days.runs / (queued + ".json")));
    ASSERT_TRUE(eventually([&] { return progress(lab, running) > 0; }));
    EXPECT_EQ(lab.cancel(running), Refusal::None);
    ASSERT_TRUE(finished(lab, running));
    const json stopped = run(lab, running);
    EXPECT_EQ(stopped["state"], "cancelled");
    EXPECT_EQ(stopped["results"]["partial"], true);
    EXPECT_EQ(stopped["results"]["digests"], nullptr);
    EXPECT_GT(stopped["results"]["events"], 0);
    EXPECT_LT(stopped["results"]["events"], kLongDay);
    EXPECT_TRUE(std::filesystem::exists(days.runs / (running + ".json")));
    EXPECT_EQ(lab.cancel(running), Refusal::Conflict);
    EXPECT_EQ(state(lab, first), "completed");
    EXPECT_EQ(lab.cancel("20261018T101500Z-0000000000000000"), Refusal::NotFound);
}

// A queued run and an idle one are erased at once, the idle one's file with it; a file it cannot remove is reported.
TEST(LabTest, DeletesAQueuedOrIdleRunAtOnce)
{
    Days days{"lab-delete"};
    Backtester lab{lab_config(), days.runs};
    const std::string idle = days.submit(lab);
    ASSERT_TRUE(finished(lab, idle));
    const std::string running = days.submit(lab, true);
    const std::string queued = days.submit(lab);
    std::string warning;
    EXPECT_EQ(lab.remove(queued, warning), Refusal::None);
    EXPECT_EQ(state(lab, queued), "gone");
    EXPECT_EQ(lab.remove(queued, warning), Refusal::NotFound);
    ASSERT_TRUE(std::filesystem::exists(days.runs / (idle + ".json")));
    EXPECT_EQ(lab.remove(idle, warning), Refusal::None);
    EXPECT_EQ(warning, "");
    EXPECT_EQ(state(lab, idle), "gone");
    EXPECT_FALSE(std::filesystem::exists(days.runs / (idle + ".json")));
    EXPECT_EQ(json::parse(lab.list())["runs"].size(), 1u);  // the long one
    ASSERT_TRUE(finished(lab, running));
    EXPECT_FALSE(std::filesystem::exists(days.runs / (queued + ".json")));

    const std::string kept = days.submit(lab);
    ASSERT_TRUE(finished(lab, kept));
    std::filesystem::permissions(days.runs, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);
    EXPECT_EQ(lab.remove(kept, warning), Refusal::None);
    std::filesystem::permissions(days.runs, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    EXPECT_EQ(warning.rfind("not removed: ", 0), 0u) << warning;
    EXPECT_EQ(state(lab, kept), "gone");
    EXPECT_TRUE(std::filesystem::exists(days.runs / (kept + ".json")));  // still there, as the warning says
}

// Review gate 5: a run deleted while it runs is hidden at once and its job cancelled; when the job stops, at its next
// market event, nothing is written or published, and the run does not come back after a restart. The run queued
// behind it completes as usual.
TEST(LabTest, ARunDeletedWhileRunningLeavesNoFileAndNeverComesBack)
{
    Days days{"lab-tombstone"};
    std::string deleted;
    std::string next;
    {
        Backtester lab{lab_config(), days.runs};
        deleted = days.submit(lab, true);
        next = days.submit(lab);
        ASSERT_TRUE(eventually([&] { return progress(lab, deleted) > 0; }));
        std::string warning;
        ASSERT_EQ(lab.remove(deleted, warning), Refusal::None);
        EXPECT_EQ(state(lab, deleted), "gone");
        EXPECT_EQ(lab.remove(deleted, warning), Refusal::NotFound);
        EXPECT_EQ(lab.cancel(deleted), Refusal::NotFound);
        ASSERT_TRUE(eventually([&] { return state(lab, next) == "completed"; }));
        EXPECT_EQ(state(lab, deleted), "gone");
        const json listed = json::parse(lab.list())["runs"];
        ASSERT_EQ(listed.size(), 1u);
        EXPECT_EQ(listed[0]["id"], next);
    }
    EXPECT_FALSE(std::filesystem::exists(days.runs / (deleted + ".json")));
    EXPECT_FALSE(std::filesystem::exists(days.runs / (deleted + ".json.tmp")));
    const Backtester restarted{lab_config(), days.runs};
    EXPECT_EQ(state(restarted, deleted), "gone");
    EXPECT_EQ(state(restarted, next), "completed");
}

// A run's file appears only once it has finished, written whole through a temp file; a restart reloads every saved
// run as it was, skips a file that is not one, and keeps runs it cannot save in memory, with a warning.
TEST(LabTest, ARunIsSavedAsItFinishesAndReloadedAfterARestart)
{
    Days days{"lab-files"};
    std::filesystem::create_directories(days.runs);
    std::ofstream{days.runs / "20261018T101500Z-9c2e41d07ab35f18.json"} << "{\"format\":1";
    std::string id;
    std::string saved;
    {
        Backtester lab{lab_config(), days.runs};
        id = days.submit(lab, true);
        bool early = false;  // a file while the job still runs
        ASSERT_TRUE(eventually([&] {
            // The file first: the state only moves on, so a file seen before a later "not done" was written early.
            const bool file = std::filesystem::exists(days.runs / (id + ".json"));
            const std::string s = state(lab, id);
            const bool done = s == "completed" || (s == "running" && progress(lab, id) == 1);
            early = early || (file && !done);
            return s == "completed";
        }));
        EXPECT_FALSE(early);
        saved = read(days.runs / (id + ".json"));
        std::string served;
        ASSERT_EQ(lab.get(id, served), Refusal::None);
        EXPECT_EQ(saved, served);
        EXPECT_EQ(json::parse(saved)["results"]["partial"], false);
        EXPECT_FALSE(std::filesystem::exists(days.runs / (id + ".json.tmp")));
    }
    const Backtester restarted{lab_config(), days.runs};
    std::string reloaded;
    ASSERT_EQ(restarted.get(id, reloaded), Refusal::None);
    EXPECT_EQ(reloaded, saved);
    EXPECT_EQ(json::parse(restarted.list())["runs"].size(), 1u);  // the corrupt file is skipped, and left there
    EXPECT_TRUE(std::filesystem::exists(days.runs / "20261018T101500Z-9c2e41d07ab35f18.json"));

    std::ofstream{days.dir.path / "not-a-directory"} << "x";
    Backtester unsaved{lab_config(), days.dir.path / "not-a-directory" / "runs"};
    const std::string kept = days.submit(unsaved);
    ASSERT_TRUE(finished(unsaved, kept));
    const json j = run(unsaved, kept);
    EXPECT_EQ(j["state"], "completed");
    EXPECT_EQ(j["warning"].get<std::string>().rfind("not saved: ", 0), 0u) << j["warning"];
}

}  // namespace
}  // namespace simulator::server::test
