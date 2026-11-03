// util::Histogram against exact percentiles from a sorted vector.
#include <gtest/gtest.h>
#include "rtes/util/histogram.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using rtes::util::Histogram;

namespace {
uint64_t exact_percentile(const std::vector<uint64_t>& sorted, double p) {
    const uint64_t rank = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size()))));
    return sorted[rank - 1];
}
} // namespace

TEST(Histogram, ExactBelow2048) {
    Histogram h;
    for (uint64_t v = 0; v < 2048; ++v) h.record(v);
    EXPECT_EQ(h.count(), 2048u);
    EXPECT_EQ(h.min(), 0u);
    EXPECT_EQ(h.max(), 2047u);
    EXPECT_EQ(h.percentile(50), 1023u);
    EXPECT_EQ(h.percentile(100), 2047u);
}

TEST(Histogram, PercentilesWithinThreeSignificantDigitsOfExact) {
    for (uint32_t seed = 1; seed <= 5; ++seed) {
        std::mt19937_64 rng(seed);
        // Log-uniform from 10 ns to 10 s: exercises many bucket ranges.
        std::uniform_real_distribution<double> exp10(1.0, 10.0);
        Histogram h;
        std::vector<uint64_t> values;
        for (int i = 0; i < 200000; ++i) {
            const auto v = static_cast<uint64_t>(std::pow(10.0, exp10(rng)));
            values.push_back(v);
            h.record(v);
        }
        std::sort(values.begin(), values.end());
        for (double p : {0.0, 1.0, 50.0, 90.0, 99.0, 99.9, 99.99, 100.0}) {
            const double exact = static_cast<double>(exact_percentile(values, p));
            const double got   = static_cast<double>(h.percentile(p));
            EXPECT_GE(got, exact) << "never under-reports, p" << p;
            EXPECT_LE(got - exact, exact / 1024.0 + 1.0) << "within 1/1024, p" << p << " seed " << seed;
        }
        EXPECT_EQ(h.max(), values.back());
        EXPECT_EQ(h.min(), values.front());
    }
}

TEST(Histogram, BucketEdgesAreConsistent) {
    // Every value maps to a bucket whose upper edge is >= the value and whose
    // previous bucket's upper edge is < the value.
    std::mt19937_64 rng(7);
    for (int i = 0; i < 100000; ++i) {
        const uint64_t v = rng() >> (rng() % 64);
        const size_t idx = Histogram::index_of(v);
        ASSERT_LT(idx, Histogram::kBuckets);
        ASSERT_GE(Histogram::upper_edge(idx), v);
        if (idx > 0) { ASSERT_LT(Histogram::upper_edge(idx - 1), v); }
    }
    EXPECT_EQ(Histogram::index_of(UINT64_MAX), Histogram::kBuckets - 1);
}

TEST(Histogram, MergeEqualsRecordingEverything) {
    Histogram a, b, all;
    for (uint64_t v = 1; v < 100000; v += 7) { a.record(v); all.record(v); }
    for (uint64_t v = 3; v < 5000000; v += 911) { b.record(v); all.record(v); }
    a.merge(b);
    EXPECT_EQ(a.count(), all.count());
    for (double p : {50.0, 99.0, 99.9, 100.0}) EXPECT_EQ(a.percentile(p), all.percentile(p));
    EXPECT_EQ(a.min(), all.min());
    EXPECT_EQ(a.max(), all.max());
}
