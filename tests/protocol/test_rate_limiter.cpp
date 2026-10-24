#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/protocol/rate_limiter.hpp"
#include "common/time/clock.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

// Explicit time points make the tests deterministic.
constexpr common::TimePoint kT0{common::Duration{1'000'000'000}};

}  // namespace

TEST_CASE(token_bucket_burst_then_rate) {
    common::TokenBucket bucket{10ms, 3U};
    CHECK(bucket.available(kT0) == 3U);
    CHECK(bucket.try_acquire(1U, kT0));
    CHECK(bucket.try_acquire(1U, kT0));
    CHECK(bucket.try_acquire(1U, kT0));
    CHECK(!bucket.try_acquire(1U, kT0));
    CHECK(bucket.available(kT0) == 0U);
    CHECK(bucket.next_available(1U, kT0) == (kT0 + 10ms));
    CHECK(bucket.next_available(2U, kT0) == (kT0 + 20ms));

    CHECK(!bucket.try_acquire(1U, kT0 + 9ms));
    CHECK(bucket.try_acquire(1U, kT0 + 10ms));
    CHECK(bucket.available(kT0 + 15ms) == 0U);
    CHECK(bucket.available(kT0 + 20ms) == 1U);

    // After a long pause the bucket is full again, but never holds more than the burst.
    const common::TimePoint later{kT0 + 1s};
    CHECK(bucket.available(later) == 3U);
    CHECK(bucket.next_available(1U, later) == later);
    CHECK(!bucket.try_acquire(4U, later));  // More than the burst can never be taken.
    CHECK(bucket.next_available(4U, later) == common::TimePoint::max());
    CHECK(bucket.try_acquire(3U, later));
    CHECK(!bucket.try_acquire(1U, later));

    bucket.reset();
    CHECK(bucket.available(later) == 3U);
}

TEST_CASE(token_bucket_per_period_never_exceeds_rate) {
    common::TokenBucket bucket{common::TokenBucket::per_period(3U, 1s, 1U)};
    CHECK(bucket.interval() == common::Duration{333'333'334});  // Rounded up.
    CHECK(bucket.burst() == 1U);

    // Try every millisecond for 10 s. Only 30 fit: 30 intervals already take more than 10 s.
    std::uint32_t acquired{0U};
    for (std::int64_t ms{0}; ms < 10'000; ++ms) {
        acquired += bucket.try_acquire(1U, kT0 + std::chrono::milliseconds{ms}) ? 1U : 0U;
    }
    CHECK(acquired == 30U);
}

TEST_CASE(sliding_window_limiter_is_exact) {
    common::SlidingWindowLimiter<3U> limiter{1s};
    CHECK(limiter.try_acquire(kT0));
    CHECK(limiter.try_acquire(kT0 + 100ms));
    CHECK(limiter.try_acquire(kT0 + 200ms));
    CHECK(!limiter.try_acquire(kT0 + 999ms));
    CHECK(limiter.count(kT0 + 999ms) == 3U);
    CHECK(limiter.next_available(kT0 + 999ms) == (kT0 + 1s));

    CHECK(limiter.try_acquire(kT0 + 1s));  // The event at kT0 has left (now - 1 s, now].
    CHECK(!limiter.try_acquire(kT0 + 1s));
    CHECK(limiter.count(kT0 + 1150ms) == 2U);  // kT0 + 200 ms and kT0 + 1 s.
    CHECK(limiter.next_available(kT0 + 1150ms) == (kT0 + 1150ms));

    limiter.reset();
    CHECK(limiter.count(kT0 + 1150ms) == 0U);
    CHECK(limiter.window() == common::Duration{1s});
    CHECK(common::SlidingWindowLimiter<3U>::max_events() == 3U);
}

TEST_CASE(sliding_window_limiter_property) {
    // Pseudo-random arrivals: no window of 100 ms may ever contain more than 5 admitted events.
    constexpr std::size_t kLimit{5U};
    const common::Duration window{100ms};
    common::SlidingWindowLimiter<kLimit> limiter{window};
    std::vector<common::TimePoint> admitted{};
    std::uint64_t state{12345U};
    common::TimePoint now{kT0};
    for (std::uint32_t i{0U}; i < 20'000U; ++i) {
        state = (state * 6'364'136'223'846'793'005U) + 1'442'695'040'888'963'407U;
        now += common::Duration{static_cast<std::int64_t>((state >> 33U) % 20'000'000U)};  // 0..20 ms
        if (limiter.try_acquire(now)) {
            admitted.push_back(now);
        }
    }
    bool within_limit{true};
    for (std::size_t i{0U}; (i + kLimit) < admitted.size(); ++i) {
        within_limit = within_limit && ((admitted[i + kLimit] - admitted[i]) >= window);
    }
    CHECK(within_limit);
    CHECK(admitted.size() > 1000U);  // The limiter is not simply rejecting everything.
}
