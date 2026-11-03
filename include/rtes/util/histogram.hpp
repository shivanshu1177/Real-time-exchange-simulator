#pragma once
// Fixed-memory latency histogram in the style of HdrHistogram.
//
// Values (e.g. nanoseconds) are recorded into log-linear buckets: exact below
// 2048, and above that each power-of-two range is split into 1024 equal
// sub-buckets. The relative error of any reported value is below 1/1024
// (three significant digits). Memory is fixed (about 450 KB), recording is
// O(1) and never allocates, so it can sit on a measured path.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace rtes::util {

class Histogram {
public:
    static constexpr int      kSubBits   = 10;                 // 1024 sub-buckets per range
    static constexpr uint64_t kSubHalf   = 1ULL << kSubBits;   // 1024
    static constexpr uint64_t kSubCount  = kSubHalf * 2;       // 2048: exact range
    static constexpr size_t   kBuckets   = kSubCount + (64 - kSubBits - 1) * kSubHalf;

    Histogram() : counts_(std::make_unique<uint64_t[]>(kBuckets)) {}

    void record(uint64_t v) noexcept {
        ++counts_[index_of(v)];
        ++count_;
        sum_ += static_cast<double>(v);
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
    }

    void merge(const Histogram& other) noexcept {
        for (size_t i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
        count_ += other.count_;
        sum_   += other.sum_;
        min_    = std::min(min_, other.min_);
        max_    = std::max(max_, other.max_);
    }

    void reset() noexcept {
        std::fill(counts_.get(), counts_.get() + kBuckets, 0);
        count_ = 0; sum_ = 0; min_ = std::numeric_limits<uint64_t>::max(); max_ = 0;
    }

    uint64_t count() const noexcept { return count_; }
    uint64_t min() const noexcept { return count_ ? min_ : 0; }
    uint64_t max() const noexcept { return max_; }
    double   mean() const noexcept { return count_ ? sum_ / static_cast<double>(count_) : 0.0; }

    // Nearest-rank percentile (p in [0, 100]). Returns the upper edge of the
    // bucket that holds that rank, capped at the true maximum, so it never
    // under-reports.
    uint64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        const double   clamped = std::clamp(p, 0.0, 100.0);
        const uint64_t rank    = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(clamped / 100.0 * static_cast<double>(count_))));
        uint64_t seen = 0;
        for (size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) return std::min(upper_edge(i), max_);
        }
        return max_;
    }

    static size_t index_of(uint64_t v) noexcept {
        if (v < kSubCount) return static_cast<size_t>(v);
        const int msb   = 63 - std::countl_zero(v);           // >= 11
        const int shift = msb - kSubBits;                      // v >> shift in [1024, 2047]
        return static_cast<size_t>(kSubCount + static_cast<uint64_t>(shift - 1) * kSubHalf +
                                   ((v >> shift) - kSubHalf));
    }

    // Largest value that maps to bucket i.
    static uint64_t upper_edge(size_t i) noexcept {
        if (i < kSubCount) return i;
        const uint64_t k     = i - kSubCount;
        const int      shift = static_cast<int>(k / kSubHalf) + 1;
        const uint64_t sub   = kSubHalf + k % kSubHalf;
        return ((sub + 1) << shift) - 1;
    }

private:
    std::unique_ptr<uint64_t[]> counts_;
    uint64_t count_{0};
    double   sum_{0};
    uint64_t min_{std::numeric_limits<uint64_t>::max()};
    uint64_t max_{0};
};

} // namespace rtes::util
