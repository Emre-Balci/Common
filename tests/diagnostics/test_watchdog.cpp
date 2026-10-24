#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include "common/core/status.hpp"
#include "common/diagnostics/watchdog.hpp"
#include "common/time/clock.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

constexpr common::TimePoint kT0{common::Duration{1'000'000'000}};

struct ExpiryLog final {
    std::uint32_t calls{0U};
    std::size_t last_index{0U};
    const char* last_name{nullptr};
};

void record_expiry(const std::size_t index, const char* const name, void* const context) noexcept {
    ExpiryLog* const log{static_cast<ExpiryLog*>(context)};
    ++log->calls;
    log->last_index = index;
    log->last_name = name;
}

}  // namespace

TEST_CASE(watchdog_detects_missing_heartbeat) {
    common::Heartbeat fast{};
    common::Heartbeat slow{};
    CHECK(fast.last_beat() == common::TimePoint::min());

    common::Watchdog<2U> watchdog{};
    const common::Result<std::size_t> a{watchdog.add(fast, 10ms, "fast", kT0)};
    const common::Result<std::size_t> b{watchdog.add(slow, 50ms, "slow", kT0)};
    REQUIRE(a.has_value() && b.has_value());
    CHECK(watchdog.add(fast, 10ms, "extra", kT0).status() == common::Status::kResourceLimit);
    CHECK(watchdog.size() == 2U);
    CHECK(std::strcmp(watchdog.name(b.value()), "slow") == 0);

    // Never beaten: supervised from registration on.
    CHECK(watchdog.check(kT0 + 10ms) == 0U);
    CHECK(watchdog.check(kT0 + 11ms) == 1U);
    CHECK(watchdog.state(a.value()) == common::Liveness::kExpired);
    CHECK(watchdog.state(b.value()) == common::Liveness::kAlive);
    CHECK(watchdog.expirations(a.value()) == 1U);

    // Beats resume: the entry recovers, and the transition count is kept.
    fast.beat(kT0 + 12ms);
    slow.beat(kT0 + 12ms);
    CHECK(watchdog.check(kT0 + 20ms) == 0U);
    CHECK(watchdog.state(a.value()) == common::Liveness::kAlive);

    CHECK(watchdog.check(kT0 + 23ms) == 1U);
    CHECK(watchdog.check(kT0 + 24ms) == 1U);  // Still expired: counted once.
    CHECK(watchdog.expirations(a.value()) == 2U);
    CHECK(watchdog.check(kT0 + 63ms) == 2U);
    CHECK(watchdog.expirations(b.value()) == 1U);

    // A beat stamped later than the supervisor's now (clock read on another CPU) is alive.
    fast.beat(kT0 + 100ms);
    CHECK(watchdog.check(kT0 + 99ms) == 1U);
    CHECK(watchdog.state(a.value()) == common::Liveness::kAlive);
}

TEST_CASE(watchdog_rejects_invalid_timeout) {
    common::Heartbeat heartbeat{};
    common::Watchdog<1U> watchdog{};
    CHECK(watchdog.add(heartbeat, 0ns, nullptr, kT0).status() == common::Status::kInvalidArgument);
    CHECK(watchdog.add(heartbeat, -1ms, nullptr, kT0).status() == common::Status::kInvalidArgument);
    const common::Result<std::size_t> entry{watchdog.add(heartbeat, 1ms, nullptr, kT0)};
    REQUIRE(entry.has_value());
    CHECK(std::strcmp(watchdog.name(entry.value()), "") == 0);
}

TEST_CASE(watchdog_expiry_handler_called_once_per_transition) {
    common::Heartbeat first{};
    common::Heartbeat second{};
    common::Watchdog<2U> watchdog{};
    ExpiryLog log{};
    watchdog.set_expiry_handler(&record_expiry, &log);
    REQUIRE(watchdog.add(first, 5ms, "first", kT0).has_value());
    REQUIRE(watchdog.add(second, 5ms, "second", kT0).has_value());

    second.beat(kT0 + 4ms);
    CHECK(watchdog.check(kT0 + 6ms) == 1U);
    CHECK(log.calls == 1U);
    CHECK(log.last_index == 0U);
    CHECK(std::strcmp(log.last_name, "first") == 0);

    CHECK(watchdog.check(kT0 + 7ms) == 1U);
    CHECK(log.calls == 1U);  // No repeat while it stays expired.

    CHECK(watchdog.check(kT0 + 10ms) == 2U);
    CHECK(log.calls == 2U);
    CHECK(log.last_index == 1U);

    watchdog.set_expiry_handler(nullptr, nullptr);
    first.beat(kT0 + 11ms);
    CHECK(watchdog.check(kT0 + 12ms) == 1U);
    CHECK(watchdog.check(kT0 + 20ms) == 2U);
    CHECK(log.calls == 2U);
}

TEST_CASE(watchdog_supervises_running_thread) {
    static common::Heartbeat heartbeat{};
    std::atomic<bool> stop{false};
    std::thread worker{[&stop]() {
        while (!stop.load(std::memory_order_acquire)) {
            heartbeat.beat();
            std::this_thread::sleep_for(1ms);
        }
    }};

    common::Watchdog<1U> watchdog{};
    const common::Result<std::size_t> entry{watchdog.add(heartbeat, 2s, "worker")};
    REQUIRE(entry.has_value());
    std::uint32_t expired{0U};
    for (std::uint32_t i{0U}; i < 20U; ++i) {
        expired += static_cast<std::uint32_t>(watchdog.check());
        std::this_thread::sleep_for(1ms);
    }
    stop.store(true, std::memory_order_release);
    worker.join();
    CHECK(expired == 0U);
    CHECK(heartbeat.last_beat() > common::TimePoint::min());

    // The thread has stopped beating: it expires once the timeout has passed.
    CHECK(watchdog.check(common::MonotonicClock::now() + 3s) == 1U);
    CHECK(watchdog.state(entry.value()) == common::Liveness::kExpired);
}
