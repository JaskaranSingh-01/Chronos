#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "simulator/cli/replay_args.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_replay.hpp"

namespace simulator {
namespace {

// Real NASDAQ data, never committed: point CHRONOS_ITCH_FILE at a TotalView-ITCH
// 5.0 file (e.g. data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin).
// CHRONOS_ITCH_SYMBOL defaults to AAPL, CHRONOS_ITCH_DATE to 2019-12-30.
std::string_view env_or(const char* name, std::string_view fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string_view{value} : fallback;
}

struct Outcome
{
    ReplayReport report;
    std::uint64_t digest{0};
    bool final_invariants{false};
    InvariantError final_error{InvariantError::None};
};

TEST(ItchRealDataTest, ReplaysOneSymbolWithoutRejections)
{
    const char* path = std::getenv("CHRONOS_ITCH_FILE");
    if (path == nullptr || *path == '\0') {
        GTEST_SKIP() << "set CHRONOS_ITCH_FILE to a NASDAQ ITCH 5.0 file";
    }

    std::chrono::year_month_day date{};
    ASSERT_TRUE(parse_date(env_or("CHRONOS_ITCH_DATE", "2019-12-30"), date));

    MappedFile file;
    ASSERT_TRUE(file.open(path)) << file.error();

    // allow_truncated_tail: a slice cut at a byte count ends mid-message.
    const ItchSourceConfig config{.ticker = env_or("CHRONOS_ITCH_SYMBOL", "AAPL"),
                                  .session_date = date,
                                  .utc_offset = us_eastern_utc_offset(date),
                                  .allow_truncated_tail = true};
    const Timestamp midnight = session_midnight_ns(date, config.utc_offset);
    const ReplayOptions options{.check_invariants_every = 10'000, .session_midnight = midnight};

    auto replay = [&] {
        InstrumentRegistry registry{1};
        ItchSource source{file.bytes(), config, registry};
        MarketBook book{0, 1'048'576};  // the registry's first id: ItchSource registers it

        Outcome run;
        run.report = MarketReplay::run(source, book, options);
        run.digest = book.digest();
        run.final_invariants = book.check_invariants(
            run.final_error, in_rth(run.report.last_timestamp, midnight));
        return run;
    };

    const Outcome run = replay();
    const ReplayReport& report = run.report;

    std::printf("[ RealData ] %.*s: %llu events, %llu applied, %llu rejected, %llu checks, digest %016llx\n",
                static_cast<int>(config.ticker.size()), config.ticker.data(),
                static_cast<unsigned long long>(report.events),
                static_cast<unsigned long long>(report.applied()),
                static_cast<unsigned long long>(report.rejected()),
                static_cast<unsigned long long>(report.invariant_checks),
                static_cast<unsigned long long>(run.digest));
    for (std::size_t type = 0; type < market_event_type_count; ++type) {
        std::printf("[ RealData ]   %-8.*s %llu\n",
                    static_cast<int>(to_string(static_cast<MarketEventType>(type)).size()),
                    to_string(static_cast<MarketEventType>(type)).data(),
                    static_cast<unsigned long long>(report.by_type[type]));
    }

    ASSERT_TRUE(report.succeeded())
        << report.message << " at position " << report.failed_position;
    EXPECT_EQ(report.rejected(), 0u);
    for (std::size_t result = 1; result < apply_result_count; ++result) {
        EXPECT_EQ(report.by_result[result], 0u)
            << to_string(static_cast<ApplyResult>(result));
    }
    EXPECT_GT(report.events, 0u);
    EXPECT_EQ(report.invariant_checks, report.applied() / options.check_invariants_every);
    EXPECT_TRUE(run.final_invariants) << to_string(run.final_error);

    // Every order-path type shows up in a liquid name, even pre-market.
    for (const MarketEventType type : {MarketEventType::Add, MarketEventType::Execute,
                                       MarketEventType::Cancel, MarketEventType::Delete,
                                       MarketEventType::Replace}) {
        EXPECT_GT(report.by_type[static_cast<std::size_t>(type)], 0u) << to_string(type);
    }

    // The replay rejects decreasing timestamps, so first and last bound them all.
    constexpr Timestamp day_ns = 86'400'000'000'000;
    EXPECT_GE(report.first_timestamp, midnight);
    EXPECT_LT(report.last_timestamp, midnight + day_ns);

    const Outcome again = replay();
    EXPECT_EQ(again.digest, run.digest);
    EXPECT_EQ(again.report, report);
}

} // namespace
} // namespace simulator
