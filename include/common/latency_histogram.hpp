/// @file latency_histogram.hpp
/// @brief Fixed-bucket latency histogram with O(1) recording.

#ifndef COMMON_LATENCY_HISTOGRAM_HPP
#define COMMON_LATENCY_HISTOGRAM_HPP

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "common/clock.hpp"

namespace common {

/// Records latencies into @p BucketCount linear buckets of @p BucketWidthNs nanoseconds.
/// The last bucket collects every sample beyond the range. Exact min / max / mean are kept.
///
/// record() is O(1), allocation-free and safe on the real-time path. Not thread-safe: the
/// histogram belongs to the recording thread; read it after that thread stops, or copy it out.
template <std::size_t BucketCount, std::int64_t BucketWidthNs>
class LatencyHistogram final {
    static_assert(BucketCount >= 2U, "BucketCount must be at least 2");
    static_assert(BucketWidthNs > 0, "BucketWidthNs must be positive");

public:
    void record(const std::int64_t latency_ns) noexcept {
        const std::int64_t value{(latency_ns < 0) ? 0 : latency_ns};
        const std::uint64_t bucket{static_cast<std::uint64_t>(value / BucketWidthNs)};
        const std::size_t index{(bucket >= BucketCount) ? (BucketCount - 1U) : static_cast<std::size_t>(bucket)};
        ++buckets_[index];
        ++count_;
        sum_ns_ += static_cast<std::uint64_t>(value);
        min_ns_ = (value < min_ns_) ? value : min_ns_;
        max_ns_ = (value > max_ns_) ? value : max_ns_;
    }

    void record(const Duration latency) noexcept {
        record(latency.count());
    }

    void reset() noexcept {
        buckets_.fill(0U);
        count_ = 0U;
        sum_ns_ = 0U;
        min_ns_ = std::numeric_limits<std::int64_t>::max();
        max_ns_ = 0;
    }

    [[nodiscard]] std::uint64_t count() const noexcept {
        return count_;
    }

    /// @return 0 when empty.
    [[nodiscard]] std::int64_t min_ns() const noexcept {
        return (count_ == 0U) ? 0 : min_ns_;
    }

    [[nodiscard]] std::int64_t max_ns() const noexcept {
        return max_ns_;
    }

    [[nodiscard]] double mean_ns() const noexcept {
        return (count_ == 0U) ? 0.0 : (static_cast<double>(sum_ns_) / static_cast<double>(count_));
    }

    /// Upper bound of the bucket containing the @p fraction quantile (e.g. 0.99), clamped to
    /// max_ns(); max_ns() when it falls in the overflow bucket. O(BucketCount): not intended for
    /// the real-time path.
    [[nodiscard]] std::int64_t percentile_ns(const double fraction) const noexcept {
        std::int64_t result{0};
        if (count_ > 0U) {
            const double clamped{(fraction < 0.0) ? 0.0 : ((fraction > 1.0) ? 1.0 : fraction)};
            std::uint64_t target{static_cast<std::uint64_t>(std::ceil(clamped * static_cast<double>(count_)))};
            target = (target == 0U) ? 1U : target;
            std::uint64_t cumulative{0U};
            std::size_t index{0U};
            while ((index < BucketCount) && (cumulative < target)) {
                cumulative += buckets_[index];
                ++index;
            }
            const std::int64_t upper{static_cast<std::int64_t>(index) * BucketWidthNs};
            result = ((index < BucketCount) && (upper < max_ns_)) ? upper : max_ns_;
        }
        return result;
    }

    [[nodiscard]] std::uint64_t bucket(const std::size_t index) const noexcept {
        return (index < BucketCount) ? buckets_[index] : 0U;
    }

    [[nodiscard]] static constexpr std::size_t bucket_count() noexcept {
        return BucketCount;
    }

    [[nodiscard]] static constexpr std::int64_t bucket_width_ns() noexcept {
        return BucketWidthNs;
    }

private:
    std::array<std::uint64_t, BucketCount> buckets_{};
    std::uint64_t count_{0U};
    std::uint64_t sum_ns_{0U};
    std::int64_t min_ns_{std::numeric_limits<std::int64_t>::max()};
    std::int64_t max_ns_{0};
};

}  // namespace common

#endif  // COMMON_LATENCY_HISTOGRAM_HPP
