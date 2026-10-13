#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "simulator/metrics/latency_histogram.hpp"

namespace simulator {
namespace {

// The bucket upper bound a percentile reports is within 1/16 above the true value.
void expect_within_bucket(std::uint64_t reported, std::uint64_t exact)
{
    EXPECT_GE(reported, exact);
    EXPECT_LE(static_cast<double>(reported), static_cast<double>(exact) * 1.0625) << "exact " << exact;
}

TEST(LatencyHistogramTest, EmptyReturnsZero)
{
    const LatencyHistogram histogram;

    EXPECT_EQ(histogram.count(), 0u);
    EXPECT_EQ(histogram.max(), 0u);
    EXPECT_EQ(histogram.percentile(0.5), 0u);
    EXPECT_EQ(histogram.percentile(1.0), 0u);
}

TEST(LatencyHistogramTest, ValuesBelowSixteenAreExact)
{
    for (std::uint64_t value = 0; value < 16; ++value) {
        LatencyHistogram histogram;
        histogram.record(value);

        EXPECT_EQ(histogram.percentile(0.5), value);
        EXPECT_EQ(histogram.max(), value);
    }
}

TEST(LatencyHistogramTest, UniformPercentilesWithinOneSixteenth)
{
    LatencyHistogram histogram;
    for (std::uint64_t value = 1; value <= 100'000; ++value) {
        histogram.record(value);
    }

    EXPECT_EQ(histogram.count(), 100'000u);
    expect_within_bucket(histogram.percentile(0.50), 50'000);
    expect_within_bucket(histogram.percentile(0.90), 90'000);
    expect_within_bucket(histogram.percentile(0.99), 99'000);
    expect_within_bucket(histogram.percentile(0.999), 99'900);
    EXPECT_EQ(histogram.percentile(1.0), 100'000u);  // clamped to the exact max
    EXPECT_EQ(histogram.max(), 100'000u);
}

TEST(LatencyHistogramTest, BimodalPercentilesFindBothModes)
{
    LatencyHistogram histogram;
    for (int i = 0; i < 900; ++i) {
        histogram.record(800);
    }
    for (int i = 0; i < 100; ++i) {
        histogram.record(50'000);
    }

    expect_within_bucket(histogram.percentile(0.50), 800);
    expect_within_bucket(histogram.percentile(0.90), 800);  // the 900th value
    expect_within_bucket(histogram.percentile(0.901), 50'000);
    expect_within_bucket(histogram.percentile(0.99), 50'000);
}

TEST(LatencyHistogramTest, HandlesTheLargestValue)
{
    constexpr std::uint64_t largest = std::numeric_limits<std::uint64_t>::max();
    LatencyHistogram histogram;
    histogram.record(1);
    histogram.record(largest);

    EXPECT_EQ(histogram.percentile(0.5), 1u);
    EXPECT_EQ(histogram.percentile(1.0), largest);
    EXPECT_EQ(histogram.max(), largest);
}

} // namespace
} // namespace simulator
