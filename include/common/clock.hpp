/// @file clock.hpp
/// @brief Monotonic time source, absolute sleeps and a drift-free periodic timer.

#ifndef COMMON_CLOCK_HPP
#define COMMON_CLOCK_HPP

#include <time.h>

#include <chrono>
#include <cstdint>
#include <ratio>

#include "common/status.hpp"

namespace common {

static_assert(sizeof(time_t) == 8U, "a 64-bit time_t is required (y2038-safe)");

/// std::chrono clock backed by CLOCK_MONOTONIC.
///
/// CLOCK_MONOTONIC never jumps (unlike CLOCK_REALTIME) and is the clock clock_nanosleep()
/// accepts for absolute deadlines. It may be slewed by NTP at a bounded rate.
/// now() is served by the vDSO: no system call, no allocation, no lock.
class MonotonicClock final {
public:
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<MonotonicClock, duration>;
    static constexpr bool is_steady{true};

    [[nodiscard]] static time_point now() noexcept {
        timespec ts{};
        // Cannot fail for a valid clock id and pointer.
        static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &ts));
        return time_point{duration{(ts.tv_sec * kNanosPerSecond) + ts.tv_nsec}};
    }

    static constexpr rep kNanosPerSecond{1'000'000'000};
};

using Duration = MonotonicClock::duration;
using TimePoint = MonotonicClock::time_point;

[[nodiscard]] constexpr timespec to_timespec(const TimePoint time) noexcept {
    const std::int64_t ns{time.time_since_epoch().count()};
    timespec ts{};
    ts.tv_sec = ns / MonotonicClock::kNanosPerSecond;
    ts.tv_nsec = ns % MonotonicClock::kNanosPerSecond;
    return ts;
}

/// Sleeps until the absolute monotonic @p deadline (returns immediately if it has passed).
/// Absolute deadlines do not accumulate drift and are immune to EINTR restarts.
/// @return kOk, kInterrupted if repeatedly interrupted by signals, or kInvalidArgument.
Status sleep_until(TimePoint deadline) noexcept;

/// Sleeps for at least @p duration.
Status sleep_for(Duration duration) noexcept;

/// Fixed-rate activation for a cyclic real-time loop.
///
/// Deadlines are computed as start + k * period, so jitter in one cycle never shifts later
/// cycles. When the loop body overruns, the timer skips the missed activations and realigns to
/// the next future boundary (no catch-up burst), and wait() reports kDeadlineMissed.
///
/// @code
///   PeriodicTimer timer{std::chrono::milliseconds{1}};
///   timer.start();
///   for (std::uint32_t cycle{0U}; cycle < kCycles; ++cycle) {
///       const Status s{timer.wait()};
///       // ... bounded work ...
///   }
/// @endcode
class PeriodicTimer final {
public:
    /// @pre period > 0
    explicit PeriodicTimer(Duration period) noexcept;

    /// Starts the schedule; the first activation is one period after @p now.
    void start(TimePoint now = MonotonicClock::now()) noexcept;

    /// Sleeps until the next activation.
    /// @return kOk, kDeadlineMissed (one or more activations were skipped), or a sleep error.
    Status wait() noexcept;

    /// Scheduled time of the most recent activation (compare with now() to measure wake-up jitter).
    [[nodiscard]] TimePoint activation_time() const noexcept {
        return activation_;
    }

    [[nodiscard]] Duration period() const noexcept {
        return period_;
    }

    /// Total number of activations skipped since start().
    [[nodiscard]] std::uint64_t missed_count() const noexcept {
        return missed_;
    }

private:
    Duration period_;
    TimePoint next_{};
    TimePoint activation_{};
    std::uint64_t missed_{0U};
};

}  // namespace common

#endif  // COMMON_CLOCK_HPP
