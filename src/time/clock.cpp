#include "common/time/clock.hpp"

#include <cerrno>
#include <cstdint>

#include "common/core/assert.hpp"

namespace common {
namespace {

// Each EINTR restart is valid with an absolute deadline; the bound only guards against a signal storm.
constexpr std::uint32_t kMaxSleepRestarts{1000U};

}  // namespace

Status sleep_until(const TimePoint deadline) noexcept {
    const timespec ts{to_timespec(deadline)};
    Status status{Status::kInterrupted};
    for (std::uint32_t attempt{0U}; attempt < kMaxSleepRestarts; ++attempt) {
        // clock_nanosleep returns the error number directly and does not set errno.
        const int rc{::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr)};
        if (rc == 0) {
            status = Status::kOk;
            break;
        }
        if (rc != EINTR) {
            status = (rc == EINVAL) ? Status::kInvalidArgument : Status::kSystemError;
            break;
        }
    }
    return status;
}

Status sleep_for(const Duration duration) noexcept {
    return sleep_until(MonotonicClock::now() + duration);
}

PeriodicTimer::PeriodicTimer(const Duration period) noexcept : period_{period} {
    COMMON_ASSERT(period.count() > 0);
}

void PeriodicTimer::start(const TimePoint now) noexcept {
    activation_ = now;
    next_ = now + period_;
    missed_ = 0U;
}

Status PeriodicTimer::wait() noexcept {
    Status result{Status::kOk};
    const TimePoint now{MonotonicClock::now()};
    if (now > next_) {
        // Skip every activation that has already passed and realign to the grid.
        const std::int64_t missed{((now - next_) / period_) + 1};
        next_ += period_ * missed;
        missed_ += static_cast<std::uint64_t>(missed);
        result = Status::kDeadlineMissed;
    }
    const Status sleep_status{sleep_until(next_)};
    activation_ = next_;
    next_ += period_;
    return is_ok(sleep_status) ? result : sleep_status;
}

}  // namespace common
