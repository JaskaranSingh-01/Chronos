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

// The run `id` once its verification has stopped.
json verified(const Backtester& lab, const std::string& id, std::size_t verifications)
{
    EXPECT_TRUE(eventually([&] {
        const json j = run(lab, id);
        return j["verifying"].is_null() && j["verifications"].size() == verifications;
    }));
    return run(lab, id);
}

// A verification re-runs a completed run with hash as read: on the same code and segments it reproduces the digests,
// and reproduced turns true. The provenance records each day's segment. While one is queued, a repeat keeps it; each completed one is recorded and saved. A run that
// is queued, running, cancelled or unknown cannot be verified.
TEST(LabTest, AVerificationOnTheSameCodeAndDaysReproducesTheRun)
{
    Days days{"lab-verify"};
    std::string id;
    {
        Backtester lab{lab_config(), days.runs};
        id = days.submit(lab);
        ASSERT_TRUE(finished(lab, id));
        EXPECT_EQ(run(lab, id)["reproduced"], nullptr);
        const std::string busy = days.submit(lab, true);
        ASSERT_TRUE(eventually([&] { return state(lab, busy) == "running"; }));
        EXPECT_EQ(lab.verify(busy, days.library), Refusal::Conflict);
        const std::string queued = days.submit(lab);
        EXPECT_EQ(lab.verify(queued, days.library), Refusal::Conflict);
        EXPECT_EQ(lab.cancel(queued), Refusal::None);
        EXPECT_EQ(lab.verify(queued, days.library), Refusal::Conflict);  // partial
        EXPECT_EQ(lab.verify("20261018T101500Z-0000000000000000", days.library), Refusal::NotFound);

        EXPECT_EQ(lab.verify(id, days.library), Refusal::None);
        EXPECT_EQ(run(lab, id)["verifying"], "queued");
        EXPECT_EQ(lab.verify(id, days.library), Refusal::None);  // the same job
        EXPECT_EQ(json::parse(lab.list())["queued"], 1);
        const json first = verified(lab, id, 1);
        const Segment& segment = *days.library.find(2019y / 12 / 27)->segment("ACME");
        EXPECT_EQ(first["provenance"]["days"][0]["segment"],
                  (json{{"hash", segment.hash}, {"size", segment.uncompressed}}));
        const json& v = first["verifications"][0];
        EXPECT_EQ(v["outcome"], "reproduced");
        EXPECT_EQ(v["digests"], first["results"]["digests"]);
        EXPECT_EQ(v["source_digest"], first["provenance"]["build"]["source_digest"]);
        EXPECT_EQ(v["changed"], json::array());
        EXPECT_EQ(first["reproduced"], true);
        EXPECT_EQ(lab.verify(id, days.library), Refusal::None);
        EXPECT_EQ(verified(lab, id, 2)["verifications"][1]["outcome"], "reproduced");
    }
    const Backtester restarted{lab_config(), days.runs};
    const json reloaded = run(restarted, id);
    EXPECT_EQ(reloaded["verifications"].size(), 2u);
    EXPECT_EQ(reloaded["reproduced"], true);
    SessionConfig lenient = lab_config();
    lenient.allow_truncated_tail = true;
    Backtester other{lenient, days.runs};  // a server started with the other --allow-truncated-tail
    EXPECT_EQ(other.verify(id, days.library), Refusal::OtherSettings);
}

// Flips a bit of the byte at `at` of `file`, in place.
void flip(const std::filesystem::path& file, std::uint64_t at)
{
    std::fstream edit{file, std::ios::in | std::ios::out | std::ios::binary};
    edit.seekg(static_cast<std::streamoff>(at));
    const char was = static_cast<char>(edit.get());
    edit.seekp(static_cast<std::streamoff>(at));
    edit.put(static_cast<char>(was ^ 0x01));
}

// Spec §4: a verification reads each segment afresh, so a store file damaged since the run is a changed input, naming
// the day, with no digests. A day rebuilt with the run's own symbol changed is refused up front.
TEST(LabTest, AVerificationOfADamagedSegmentIsInputChangedAndOfARebuiltOneRefused)
{
    Days days{"lab-damaged"};
    Backtester lab{lab_config(), days.runs};
    const std::string id = days.submit(lab);
    ASSERT_TRUE(finished(lab, id));
    const Segment friday = *days.library.find(2019y / 12 / 27)->segment("ACME");
    flip(days.dir.path / "data" / "store" / "ACME.itch.zst", friday.offset + friday.compressed - 1);  // its checksum
    ASSERT_EQ(lab.verify(id, days.library), Refusal::None);
    const json j = verified(lab, id, 1);
    EXPECT_EQ(j["verifications"][0]["outcome"], "input_changed");
    EXPECT_EQ(j["verifications"][0]["changed"], json::parse(R"(["2019-12-27"])"));
    EXPECT_EQ(j["verifications"][0]["digests"], nullptr);
    EXPECT_EQ(j["reproduced"], nullptr);

    std::ofstream{days.dir.path / "data" / "12272019.NASDAQ_ITCH50.bin", std::ios::app | std::ios::binary} << 'x';
    days.library.scan();  // rebuilt: the cut tail is in every segment of the day, ACME's too
    EXPECT_EQ(lab.verify(id, days.library), Refusal::InputChanged);
}

// Spec §4 and §6, symbol isolation: another symbol's message changed in the day file rebuilds the day, but the run's
// segment keeps its bytes and hash, so the run still verifies as reproduced.
TEST(LabTest, AnotherSymbolsChangedMessageLeavesARunReproducible)
{
    const ScratchDir dir{"lab-isolation"};
    const auto friday = [](std::uint32_t zzz) {
        itch_test::ItchBuilder itch = lab_day(1);
        itch.stock_directory(2, 17 * 3'600 * kSecond, "ZZZ")
            .add_order(2, 17 * 3'600 * kSecond, 9, 'B', 100, "ZZZ", zzz);
        return itch;
    };
    const std::filesystem::path file = dir.path / "12272019.NASDAQ_ITCH50.bin";
    write_file(file, friday(100'000).bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    const DayHandle before = library.scan().at(0);
    Backtester lab{lab_config(), dir.path / "runs"};
    RunSpec spec = default_spec(SessionStrategyKind::MarketMaker);
    spec.ticker = ticker("ACME");
    spec.days = {.day = {2019y / 12 / 27}, .count = 1};
    std::string id;
    ASSERT_EQ(lab.submit(spec, {before}, id), Refusal::None);
    ASSERT_TRUE(finished(lab, id));

    write_file(file, friday(100'100).bytes());  // ZZZ's price, the same size
    const DayHandle after = library.scan().at(0);
    EXPECT_NE(after->hash, before->hash);
    EXPECT_EQ(after->segment("ACME")->hash, before->segment("ACME")->hash);  // a new copy of the same bytes
    EXPECT_GT(after->segment("ACME")->offset, before->segment("ACME")->offset);
    EXPECT_NE(after->segment("ZZZ")->hash, before->segment("ZZZ")->hash);
    ASSERT_EQ(lab.verify(id, library), Refusal::None);
    EXPECT_EQ(verified(lab, id, 1)["verifications"][0]["outcome"], "reproduced");
}

// Spec §4: a run saved before the per-symbol store has no segment hashes. It stays listed and viewable, but neither a
// verification nor a Watch claims it against an identity it did not record.
TEST(LabTest, ARunRecordedBeforeTheStoreIsListedButNeitherVerifiedNorWatched)
{
    Days days{"lab-before-store"};
    std::string id;
    {
        Backtester lab{lab_config(), days.runs};
        id = days.submit(lab);
        ASSERT_TRUE(finished(lab, id));
    }
    const std::filesystem::path file = days.runs / (id + ".json");
    json saved = json::parse(read(file));
    for (json& day : saved["provenance"]["days"]) {
        day.erase("segment");
    }
    std::ofstream{file} << saved.dump();

    Backtester lab{lab_config(), days.runs};
    EXPECT_EQ(state(lab, id), "completed");
    EXPECT_EQ(run(lab, id)["provenance"]["days"][0]["segment"], nullptr);
    EXPECT_EQ(lab.verify(id, days.library), Refusal::BeforeStore);
    Control control;
    ASSERT_TRUE(to_padded(id, control.watch.run_id));
    std::string why;
    EXPECT_FALSE(lab.watch(control, days.library, why));
    EXPECT_EQ(why, id + ": recorded before the per-symbol store: queue it again");
}

// Review gate 2, Verify's half: a run recorded by a build of other source, as a dirty tree at the same commit would
// be, verifies as different_code, never as a failed reproduction.
TEST(LabTest, AVerificationOnOtherCodeReportsDifferentCode)
{
    Days days{"lab-other-code"};
    std::string id;
    {
        Backtester lab{lab_config(), days.runs};
        id = days.submit(lab);
        ASSERT_TRUE(finished(lab, id));
    }
    const std::filesystem::path file = days.runs / (id + ".json");
    json saved = json::parse(read(file));
    saved["provenance"]["build"]["source_digest"] = std::string(64, '0');
    std::ofstream{file} << saved.dump();

    Backtester lab{lab_config(), days.runs};
    ASSERT_EQ(lab.verify(id, days.library), Refusal::None);
    const json j = verified(lab, id, 1);
    EXPECT_EQ(j["verifications"][0]["outcome"], "different_code");
    EXPECT_NE(j["verifications"][0]["source_digest"], std::string(64, '0'));
    EXPECT_EQ(j["verifications"][0]["digests"], j["results"]["digests"]);
    EXPECT_EQ(j["reproduced"], nullptr);
}

}  // namespace
}  // namespace simulator::server::test
