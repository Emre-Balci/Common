#include <chrono>
#include <cstdint>

#include "common/clock.hpp"
#include "common/latency_histogram.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

TEST_CASE(clock_is_monotonic) {
    common::TimePoint previous{common::MonotonicClock::now()};
    bool monotonic{true};
    for (std::uint32_t i{0U}; i < 100'000U; ++i) {
        const common::TimePoint now{common::MonotonicClock::now()};
        monotonic = monotonic && (now >= previous);
        previous = now;
    }
    CHECK(monotonic);
}

TEST_CASE(sleep_until_never_wakes_early) {
    const common::TimePoint deadline{common::MonotonicClock::now() + 2ms};
    CHECK(common::is_ok(common::sleep_until(deadline)));
    CHECK(common::MonotonicClock::now() >= deadline);
    // A deadline in the past returns immediately.
    CHECK(common::is_ok(common::sleep_until(deadline - 1s)));
}

TEST_CASE(periodic_timer_keeps_grid) {
    common::PeriodicTimer timer{1ms};
    const common::TimePoint start{common::MonotonicClock::now()};
    timer.start(start);
    for (std::int64_t cycle{1}; cycle <= 5; ++cycle) {
        const common::Status status{timer.wait()};
        CHECK((status == common::Status::kOk) || (status == common::Status::kDeadlineMissed));
        // Activations stay on the start + k * period grid whatever the wake-up jitter.
        CHECK(((timer.activation_time() - start) % 1ms) == 0ns);
        CHECK(common::MonotonicClock::now() >= timer.activation_time());
    }
}

TEST_CASE(periodic_timer_reports_overrun) {
    common::PeriodicTimer timer{1ms};
    timer.start();
    CHECK(common::is_ok(common::sleep_for(5ms)));  // Simulated overrun of the loop body.
    CHECK(timer.wait() == common::Status::kDeadlineMissed);
    CHECK(timer.missed_count() >= 4U);
    CHECK(timer.activation_time() > (common::MonotonicClock::now() - 1ms));
}

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
