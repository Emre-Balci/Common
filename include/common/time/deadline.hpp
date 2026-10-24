/// @file deadline.hpp
/// @brief An absolute point in monotonic time by which an operation must finish.

#ifndef COMMON_TIME_DEADLINE_HPP
#define COMMON_TIME_DEADLINE_HPP

#include <chrono>

#include "common/time/clock.hpp"

namespace common {

/// Absolute monotonic deadline for an operation made of several steps, e.g. a request whose
/// response arrives in pieces. Each step waits for remaining() instead of the full timeout, so the
/// steps together never exceed it:
/// @code
///   const common::Deadline deadline{common::Deadline::after(std::chrono::milliseconds{200})};
///   for (std::uint32_t step{0U}; (step < kMaxSteps) && (!done) && (!deadline.expired()); ++step) {
///       const common::Status status{port.read(buffer, deadline)};  // Waits at most until the deadline.
///       // ...
///   }
/// @endcode
///
/// A Deadline is a trivially copyable value; reading the clock is the only thing it does, so every
/// member is safe on the real-time path.
class Deadline final {
public:
    /// A deadline that has already expired.
    constexpr Deadline() noexcept = default;

    /// The deadline @p timeout after @p now. A zero or negative timeout gives a deadline that has
    /// expired, and a timeout too large for the clock gives never().
    [[nodiscard]] static Deadline after(const Duration timeout, const TimePoint now = MonotonicClock::now()) noexcept {
        Deadline deadline{now};
        if (timeout > Duration::zero()) {
            // TimePoint::max() - timeout cannot overflow for a positive timeout.
            deadline.time_ = (now < (TimePoint::max() - timeout)) ? (now + timeout) : TimePoint::max();
        }
        return deadline;
    }

    /// The deadline at the absolute time @p time.
    [[nodiscard]] static constexpr Deadline at(const TimePoint time) noexcept {
        return Deadline{time};
    }

    /// A deadline that never expires, for operations that may wait indefinitely.
    [[nodiscard]] static constexpr Deadline never() noexcept {
        return Deadline{TimePoint::max()};
    }

    [[nodiscard]] constexpr bool is_never() const noexcept {
        return time_ == TimePoint::max();
    }

    /// @return true once @p now has reached the deadline.
    [[nodiscard]] bool expired(const TimePoint now = MonotonicClock::now()) const noexcept {
        return (!is_never()) && (now >= time_);
    }

    /// @return the time left until the deadline, zero once it has expired; Duration::max() for never().
    [[nodiscard]] Duration remaining(const TimePoint now = MonotonicClock::now()) const noexcept {
        Duration left{Duration::zero()};
        if (is_never()) {
            left = Duration::max();
        } else if (now < time_) {
            left = time_ - now;
        } else {
            // Expired: no time left.
        }
        return left;
    }

    [[nodiscard]] constexpr TimePoint time_point() const noexcept {
        return time_;
    }

private:
    constexpr explicit Deadline(const TimePoint time) noexcept : time_{time} {}

    TimePoint time_{TimePoint::min()};
};

}  // namespace common

#endif  // COMMON_TIME_DEADLINE_HPP
