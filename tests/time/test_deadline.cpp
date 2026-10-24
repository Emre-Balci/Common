#include <chrono>

#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

// Explicit time points make the tests deterministic.
constexpr common::TimePoint kT0{common::Duration{1'000'000'000}};

}  // namespace

TEST_CASE(deadline_after_and_remaining) {
    const common::Deadline deadline{common::Deadline::after(100ms, kT0)};
    CHECK(deadline.time_point() == (kT0 + 100ms));
    CHECK(!deadline.is_never());
    CHECK(!deadline.expired(kT0));
    CHECK(!deadline.expired(kT0 + 99ms));
    CHECK(deadline.expired(kT0 + 100ms));
    CHECK(deadline.remaining(kT0) == 100ms);
    CHECK(deadline.remaining(kT0 + 30ms) == 70ms);
    CHECK(deadline.remaining(kT0 + 100ms) == common::Duration::zero());
    CHECK(deadline.remaining(kT0 + 1s) == common::Duration::zero());  // Never negative.

    CHECK(common::Deadline::at(kT0).expired(kT0));
    CHECK(common::Deadline::at(kT0).time_point() == kT0);
}

TEST_CASE(deadline_edge_cases) {
    // A zero or negative timeout has already expired.
    CHECK(common::Deadline::after(0ms, kT0).expired(kT0));
    CHECK(common::Deadline::after(-5ms, kT0).expired(kT0));
    CHECK(common::Deadline::after(-5ms, kT0).remaining(kT0) == common::Duration::zero());
    CHECK(common::Deadline{}.expired(kT0));

    // A timeout too large for the clock saturates to never().
    const common::Deadline huge{common::Deadline::after(common::Duration::max(), kT0)};
    CHECK(huge.is_never());
    CHECK(!huge.expired(common::TimePoint::max()));

    const common::Deadline never{common::Deadline::never()};
    CHECK(never.is_never());
    CHECK(!never.expired(kT0 + 1000h));
    CHECK(never.remaining(kT0) == common::Duration::max());

    // With the real clock: a fresh short deadline has not expired yet.
    const common::Deadline real{common::Deadline::after(10s)};
    CHECK(!real.expired());
    CHECK(real.remaining() > 9s);
}
