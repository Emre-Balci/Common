#include "common/protocol/rate_limiter.hpp"

#include <cstdint>

#include "common/core/assert.hpp"
#include "common/numeric/integer.hpp"

namespace common {
namespace {

// Keeps tat_ + cost and similar sums far below the Duration limit for any realistic clock value.
constexpr std::int64_t kMaxCapacityNs{1'152'921'504'606'846'976};  // 2^60 ns, about 36 years

Duration capacity_of(const Duration interval, const std::uint32_t burst) noexcept {
    COMMON_ASSERT(interval.count() > 0);
    COMMON_ASSERT(burst > 0U);
    const Result<std::int64_t> capacity{checked_mul(interval.count(), static_cast<std::int64_t>(burst))};
    COMMON_ASSERT(capacity.has_value() && (capacity.value() <= kMaxCapacityNs));
    return Duration{capacity.value()};
}

}  // namespace

TokenBucket::TokenBucket(const Duration interval, const std::uint32_t burst) noexcept
    : interval_{interval}, burst_{burst}, capacity_{capacity_of(interval, burst)} {}

TokenBucket TokenBucket::per_period(const std::uint32_t tokens, const Duration period,
                                    const std::uint32_t burst) noexcept {
    COMMON_ASSERT(tokens > 0U);
    COMMON_ASSERT(period.count() >= static_cast<std::int64_t>(tokens));
    // The guard keeps the division defined even when assertions are disabled.
    const std::int64_t divisor{(tokens > 0U) ? static_cast<std::int64_t>(tokens) : 1};
    std::int64_t interval_ns{period.count() / divisor};
    if ((period.count() % divisor) != 0) {
        ++interval_ns;  // Round up: a shorter interval would exceed the limit.
    }
    return TokenBucket{Duration{interval_ns}, burst};
}

bool TokenBucket::try_acquire(const std::uint32_t tokens, const TimePoint now) noexcept {
    bool acquired{false};
    if (tokens <= burst_) {
        const Duration cost{interval_ * static_cast<std::int64_t>(tokens)};
        const TimePoint base{(tat_ > now) ? tat_ : now};
        const TimePoint new_tat{base + cost};
        if ((new_tat - now) <= capacity_) {
            tat_ = new_tat;
            acquired = true;
        }
    }
    return acquired;
}

std::uint32_t TokenBucket::available(const TimePoint now) const noexcept {
    std::uint32_t tokens{burst_};
    if (tat_ > now) {
        const Duration used{tat_ - now};
        // used can only exceed capacity_ if the caller's clock went backwards.
        tokens = (used >= capacity_) ? 0U : static_cast<std::uint32_t>((capacity_ - used) / interval_);
    }
    return tokens;
}

TimePoint TokenBucket::next_available(const std::uint32_t tokens, const TimePoint now) const noexcept {
    TimePoint result{TimePoint::max()};
    if (tokens <= burst_) {
        result = now;
        if (tat_ > now) {
            // try_acquire() succeeds at t when tat_ + cost - t <= capacity_.
            const Duration cost{interval_ * static_cast<std::int64_t>(tokens)};
            const TimePoint earliest{tat_ - (capacity_ - cost)};
            result = (earliest > now) ? earliest : now;
        }
    }
    return result;
}

}  // namespace common
