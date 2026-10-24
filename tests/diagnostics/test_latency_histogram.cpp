#include <cstdint>

#include "common/diagnostics/latency_histogram.hpp"
#include "test_framework.hpp"

TEST_CASE(latency_histogram_statistics) {
    common::LatencyHistogram<100U, 1000> histogram{};
    CHECK(histogram.percentile_ns(0.5) == 0);
    for (std::int64_t i{1}; i <= 100; ++i) {
        histogram.record(i * 1000);
    }
    histogram.record(10'000'000);  // Lands in the overflow bucket.
    CHECK(histogram.count() == 101U);
    CHECK(histogram.min_ns() == 1000);
    CHECK(histogram.max_ns() == 10'000'000);
    CHECK(histogram.percentile_ns(0.5) == 52'000);  // 51st sample (51 us) lies in [51, 52) us.
    CHECK(histogram.percentile_ns(1.0) == 10'000'000);
    CHECK(histogram.bucket(99U) == 3U);  // 99 us, 100 us and the 10 ms outlier.
    histogram.reset();
    CHECK(histogram.count() == 0U);
}
