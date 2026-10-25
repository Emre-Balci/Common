#include "common/time/clock.hpp"

#include <cerrno>
#include <cstdint>

#include "common/core/assert.hpp"

#if defined(_WIN32)
#include "detail/win32.hpp"
#endif

namespace common {
namespace {

// Each EINTR restart is valid with an absolute deadline; the bound only guards against a signal storm.
// On Windows it bounds the re-arming of the timer after a wake-up that came before the deadline.
constexpr std::uint32_t kMaxSleepRestarts{1000U};

#if defined(_WIN32)
// Waitable timers count in units of 100 ns.
constexpr std::int64_t kNanosPerTimerTick{100};

/// A thread's waitable timer, closed when the thread exits.
class ThreadTimer final {
public:
    ThreadTimer() noexcept
        // A high-resolution timer (Windows 10 1803 and later) wakes within about half a millisecond
        // instead of at the next system tick (1 to 16 ms); older versions get a standard timer.
        : handle_{::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)} {
        if (handle_ == nullptr) {
            handle_ = ::CreateWaitableTimerExW(nullptr, nullptr, 0U, TIMER_ALL_ACCESS);
        }
    }
    ~ThreadTimer() {
        if (handle_ != nullptr) {
            static_cast<void>(::CloseHandle(handle_));
        }
    }
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer(ThreadTimer&&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;
    ThreadTimer& operator=(ThreadTimer&&) = delete;

    /// @return the timer, or nullptr if it could not be created.
    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_;
};
#endif

}  // namespace

#if defined(_WIN32)
Status sleep_until(const TimePoint deadline) noexcept {
    // Created on the thread's first call, so that later calls neither allocate nor fail.
    thread_local const ThreadTimer timer{};
    Status status{(timer.get() != nullptr) ? Status::kInterrupted : Status::kResourceLimit};
    // Each round sleeps for the time that is left. A relative due time follows the interrupt clock,
    // which can run a little apart from the performance counter: a wake-up that MonotonicClock still
    // sees before the deadline sleeps again.
    for (std::uint32_t attempt{0U}; (status == Status::kInterrupted) && (attempt < kMaxSleepRestarts); ++attempt) {
        const Duration remaining{deadline - MonotonicClock::now()};
        if (remaining <= Duration::zero()) {
            status = Status::kOk;
        } else {
            // A negative due time is relative, and immune to changes of the system time. Rounding up
            // to whole ticks keeps the wake-up from coming early.
            const std::int64_t ticks{(remaining.count() / kNanosPerTimerTick) +
                                     (((remaining.count() % kNanosPerTimerTick) != 0) ? 1 : 0)};
            const LARGE_INTEGER due{detail::large_integer(-ticks)};
            if ((::SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE) == FALSE) ||
                (::WaitForSingleObject(timer.get(), INFINITE) != WAIT_OBJECT_0)) {
                status = Status::kSystemError;
            }
        }
    }
    return status;
}
#else
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
#endif

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
