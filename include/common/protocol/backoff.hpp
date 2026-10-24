/// @file backoff.hpp
/// @brief Bounded exponential backoff for retrying an operation, e.g. reconnecting to a device.

#ifndef COMMON_PROTOCOL_BACKOFF_HPP
#define COMMON_PROTOCOL_BACKOFF_HPP

#include <cstdint>

#include "common/core/assert.hpp"
#include "common/core/status.hpp"
#include "common/time/clock.hpp"

namespace common {

/// Delays between the attempts of a retried operation: the first delay is @p initial, each later
/// one twice the previous, capped at @p maximum; after @p max_attempts delays the retries are
/// exhausted. Doubling quickly backs off from a device or peer that is down without flooding it,
/// and the cap bounds the time to notice that it is back.
///
/// The number of attempts is always bounded, so a retry loop has a fixed upper bound. To keep
/// trying forever (e.g. a supervisor reconnecting a removable adapter), report the exhaustion,
/// then reset() and start over.
/// @code
///   common::ExponentialBackoff backoff{std::chrono::milliseconds{100}, std::chrono::seconds{5}, 10U};
///   Status status{connect()};
///   while (!common::is_ok(status)) {
///       const common::Result<common::Duration> delay{backoff.next_delay()};
///       if (!delay.has_value()) { break; }  // Retries exhausted: give up and report.
///       static_cast<void>(common::sleep_for(delay.value()));
///       status = connect();
///   }
///   backoff.reset();  // Connected: the next failure starts with the short delay again.
/// @endcode
/// The loop runs at most max_attempts + 1 times, because next_delay() fails after max_attempts calls.
///
/// No jitter is added, so the delays are deterministic. Every member is O(1) and safe on the
/// real-time path; the waiting itself is the caller's (sleep, or a timer in an event loop).
/// Not thread-safe.
class ExponentialBackoff final {
public:
    /// @pre initial > 0, maximum >= initial, max_attempts > 0
    ExponentialBackoff(const Duration initial, const Duration maximum, const std::uint32_t max_attempts) noexcept
        : initial_{initial}, maximum_{maximum}, next_{initial}, max_attempts_{max_attempts} {
        COMMON_ASSERT(initial > Duration::zero());
        COMMON_ASSERT(maximum >= initial);
        COMMON_ASSERT(max_attempts > 0U);
    }

    /// @return the delay before the next attempt; kResourceLimit once max_attempts delays were
    ///         handed out (until reset()).
    [[nodiscard]] Result<Duration> next_delay() noexcept {
        Result<Duration> result{Result<Duration>::failure(Status::kResourceLimit)};
        if (attempts_ < max_attempts_) {
            result = Result<Duration>::success(next_);
            ++attempts_;
            // Doubles without overflow: next_ <= maximum_ / 2 rules out wrapping.
            next_ = (next_ <= (maximum_ / 2)) ? (next_ * 2) : maximum_;
        }
        return result;
    }

    /// Starts over with the initial delay, e.g. after the operation succeeded.
    void reset() noexcept {
        next_ = initial_;
        attempts_ = 0U;
    }

    /// Number of delays handed out since construction or the last reset().
    [[nodiscard]] std::uint32_t attempts() const noexcept {
        return attempts_;
    }

    [[nodiscard]] bool exhausted() const noexcept {
        return attempts_ >= max_attempts_;
    }

private:
    Duration initial_;
    Duration maximum_;
    Duration next_;
    std::uint32_t max_attempts_;
    std::uint32_t attempts_{0U};
};

}  // namespace common

#endif  // COMMON_PROTOCOL_BACKOFF_HPP
