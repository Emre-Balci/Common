/// @file rate_limiter.hpp
/// @brief Rate limiting on monotonic time: an O(1) token bucket and an exact sliding window.

#ifndef COMMON_PROTOCOL_RATE_LIMITER_HPP
#define COMMON_PROTOCOL_RATE_LIMITER_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/ring_buffer.hpp"
#include "common/core/assert.hpp"
#include "common/time/clock.hpp"

namespace common {

/// Token bucket in its GCRA (generic cell rate algorithm) form: one token is added every
/// interval(), up to burst() tokens. The whole state is one time point, so every operation is
/// O(1), with no loop and no division on the acquire path.
///
/// Use it to smooth an average rate, e.g. to pace outgoing CAN frames or orders. A token bucket
/// can admit up to 2 * burst - 1 events in a window of burst * interval; when a counterparty
/// enforces "at most N events in any window", use SlidingWindowLimiter.
///
/// Not thread-safe: the limiter belongs to one thread.
class TokenBucket final {
public:
    /// One token every @p interval, at most @p burst stored. The bucket starts full.
    /// @pre interval > 0, burst > 0, and interval * burst must not exceed 2^60 ns (about 36 years).
    TokenBucket(Duration interval, std::uint32_t burst) noexcept;

    /// At most @p tokens per @p period on average, with up to @p burst at once. The interval is
    /// rounded up to whole nanoseconds, so the long-run rate never exceeds the limit.
    /// @pre tokens > 0, period >= tokens nanoseconds
    [[nodiscard]] static TokenBucket per_period(std::uint32_t tokens, Duration period, std::uint32_t burst) noexcept;

    /// Takes @p tokens if they are all available.
    /// @return false if not (nothing is taken); always false when tokens > burst().
    [[nodiscard]] bool try_acquire(std::uint32_t tokens = 1U, TimePoint now = MonotonicClock::now()) noexcept;

    /// Number of tokens that try_acquire() could take at @p now.
    [[nodiscard]] std::uint32_t available(TimePoint now = MonotonicClock::now()) const noexcept;

    /// Earliest time at which try_acquire(@p tokens) succeeds (@p now if it already would).
    /// @return TimePoint::max() when tokens > burst().
    [[nodiscard]] TimePoint next_available(std::uint32_t tokens = 1U,
                                           TimePoint now = MonotonicClock::now()) const noexcept;

    /// Refills the bucket.
    void reset() noexcept {
        tat_ = TimePoint::min();
    }

    [[nodiscard]] Duration interval() const noexcept {
        return interval_;
    }

    [[nodiscard]] std::uint32_t burst() const noexcept {
        return burst_;
    }

private:
    Duration interval_;
    std::uint32_t burst_;
    Duration capacity_;  // interval_ * burst_
    // Theoretical arrival time: the bucket is full while tat_ <= now, and empty at now + capacity_.
    TimePoint tat_{TimePoint::min()};
};

/// Admits at most @p MaxEvents events in any time window of a fixed length: an event at @p now
/// is admitted when fewer than MaxEvents admitted events lie in (now - window, now].
///
/// This is the rule exchanges and bus specifications usually state ("N messages per second"),
/// and unlike a token bucket it is never exceeded by a burst. It keeps the timestamps of the
/// last MaxEvents admitted events, so try_acquire() is O(1) and needs O(MaxEvents) memory.
///
/// Not thread-safe: the limiter belongs to one thread.
template <std::size_t MaxEvents>
class SlidingWindowLimiter final {
    static_assert(MaxEvents > 0U, "MaxEvents must be positive");

public:
    /// @pre window > 0
    explicit SlidingWindowLimiter(const Duration window) noexcept : window_{window} {
        COMMON_ASSERT(window.count() > 0);
    }

    /// Records an event if the limit allows it. @return false if not (nothing is recorded).
    [[nodiscard]] bool try_acquire(const TimePoint now = MonotonicClock::now()) noexcept {
        bool admitted{false};
        // When full, the oldest of the last MaxEvents events decides whether one more fits.
        if ((!history_.full()) || ((now - to_time_point(history_.front())) >= window_)) {
            static_cast<void>(history_.push_overwrite(now.time_since_epoch().count()));
            admitted = true;
        }
        return admitted;
    }

    /// Number of admitted events inside the window ending at @p now. O(MaxEvents).
    [[nodiscard]] std::size_t count(const TimePoint now = MonotonicClock::now()) const noexcept {
        std::size_t first{0U};
        while ((first < history_.size()) && ((now - to_time_point(history_[first])) >= window_)) {
            ++first;
        }
        return history_.size() - first;
    }

    /// Earliest time at which try_acquire() succeeds (@p now if it already would).
    [[nodiscard]] TimePoint next_available(const TimePoint now = MonotonicClock::now()) const noexcept {
        TimePoint result{now};
        if (history_.full()) {
            const TimePoint expiry{to_time_point(history_.front()) + window_};
            result = (expiry > now) ? expiry : now;
        }
        return result;
    }

    /// Forgets every recorded event.
    void reset() noexcept {
        history_.clear();
    }

    [[nodiscard]] Duration window() const noexcept {
        return window_;
    }

    [[nodiscard]] static constexpr std::size_t max_events() noexcept {
        return MaxEvents;
    }

private:
    static TimePoint to_time_point(const std::int64_t nanoseconds) noexcept {
        return TimePoint{Duration{nanoseconds}};
    }

    // Admitted timestamps in nanoseconds, oldest first. Stored as integers because the default
    // constructor of std::chrono::time_point is not noexcept, which RingBuffer requires.
    // Declared before window_: MSVC 19.40 (VS 2022 17.10) at /O2 loses the value of window_ when
    // window_ comes first and the zero-initialised history_ follows it (an SSA optimizer bug).
    RingBuffer<std::int64_t, MaxEvents> history_{};
    Duration window_;
};

}  // namespace common

#endif  // COMMON_PROTOCOL_RATE_LIMITER_HPP
