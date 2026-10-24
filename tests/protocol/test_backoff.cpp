#include <chrono>
#include <cstdint>

#include "common/protocol/backoff.hpp"
#include "common/time/clock.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

TEST_CASE(backoff_doubles_up_to_the_maximum) {
    common::ExponentialBackoff backoff{100ms, 1s, 6U};
    CHECK(backoff.attempts() == 0U);
    CHECK(!backoff.exhausted());
    CHECK(backoff.next_delay().value() == 100ms);
    CHECK(backoff.next_delay().value() == 200ms);
    CHECK(backoff.next_delay().value() == 400ms);
    CHECK(backoff.next_delay().value() == 800ms);
    CHECK(backoff.next_delay().value() == 1s);  // Capped.
    CHECK(backoff.next_delay().value() == 1s);
    CHECK(backoff.attempts() == 6U);
    CHECK(backoff.exhausted());
    const common::Result<common::Duration> exhausted{backoff.next_delay()};
    CHECK(!exhausted.has_value());
    CHECK(exhausted.status() == common::Status::kResourceLimit);

    backoff.reset();
    CHECK(backoff.attempts() == 0U);
    CHECK(backoff.next_delay().value() == 100ms);
}

TEST_CASE(backoff_never_overflows) {
    common::ExponentialBackoff backoff{common::Duration::max() / 3, common::Duration::max(), 4U};
    CHECK(backoff.next_delay().value() == (common::Duration::max() / 3));
    CHECK(backoff.next_delay().value() == ((common::Duration::max() / 3) * 2));
    CHECK(backoff.next_delay().value() == common::Duration::max());
    CHECK(backoff.next_delay().value() == common::Duration::max());

    common::ExponentialBackoff fixed{50ms, 50ms, 2U};
    CHECK(fixed.next_delay().value() == 50ms);
    CHECK(fixed.next_delay().value() == 50ms);
    CHECK(!fixed.next_delay().has_value());
}
