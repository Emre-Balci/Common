/// @file task_scheduler.hpp
/// @brief Cyclic executive: runs periodic and triggered tasks in priority order on one thread.

#ifndef COMMON_TASK_TASK_SCHEDULER_HPP
#define COMMON_TASK_TASK_SCHEDULER_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "common/containers/span.hpp"
#include "common/core/assert.hpp"
#include "common/core/status.hpp"
#include "common/diagnostics/watchdog.hpp"
#include "common/numeric/integer.hpp"
#include "common/system/thread.hpp"
#include "common/task/task.hpp"
#include "common/time/clock.hpp"

namespace common {

/// When and how a TaskScheduler activates a task.
struct TaskSchedule final {
    Duration period{};           ///< > 0: activated every period. Zero requires triggered_only.
    Duration offset{};           ///< First activation at the grid origin + offset (spreads equal-period tasks); >= 0.
    Duration budget{};           ///< Execution time above which a run counts as an overrun; zero: no budget.
    Duration deadline{};         ///< Relative to the release; zero: the period (none for a triggered-only task).
    std::uint8_t priority{0U};   ///< Order among the tasks due in one pass: higher first, ties in add() order.
                                 ///< It never preempts a running task.
    bool triggered_only{false};  ///< Runs only when triggered (the period must be zero).
    bool suspended{false};       ///< Added suspended, e.g. to be resumed in a unit's on_start().

    /// A periodic schedule.
    [[nodiscard]] static constexpr TaskSchedule periodic(const Duration period, const Duration offset = Duration{}) noexcept {
        TaskSchedule schedule{};
        schedule.period = period;
        schedule.offset = offset;
        return schedule;
    }

    /// A schedule that runs only when triggered.
    [[nodiscard]] static constexpr TaskSchedule triggered() noexcept {
        TaskSchedule schedule{};
        schedule.triggered_only = true;
        return schedule;
    }
};

namespace detail {
namespace task_scheduler {

/// A task of a scheduler.
struct Entry final {
    Task* task{nullptr};
    TaskSchedule schedule{};
    std::int64_t next_ns{0};  ///< First grid point not yet released (periodic tasks, once aligned).
    bool triggered{false};    ///< A trigger taken at the start of the current pass.
};

/// The time base of a scheduler.
struct Grid final {
    std::int64_t origin_ns{0};  ///< Grid origin set by align().
    std::int64_t last_ns{0};    ///< The now of the last align() or pass.
    bool aligned{false};
};

/// No activation (a saturated or absent next activation).
inline constexpr std::int64_t kNever{std::numeric_limits<std::int64_t>::max()};

/// @return kOk, or kInvalidArgument for a negative period, offset, budget or deadline, a zero period
///         without triggered_only, or triggered_only with a period.
[[nodiscard]] Status validate(const TaskSchedule& schedule) noexcept;

/// @return the first grid point origin + offset + j * period (j >= 0) at or after @p time_ns, saturating.
[[nodiscard]] std::int64_t first_point_at_or_after(const TaskSchedule& schedule, std::int64_t origin_ns,
                                                   std::int64_t time_ns) noexcept;

/// Sets the grid origin and puts every periodic task at origin + offset.
void align(Span<Entry> entries, Grid& grid, std::int64_t origin_ns) noexcept;

/// Applies the tasks' suspension requests at @p now_ns (kScheduled <-> kSuspended).
void apply_suspensions(Span<Entry> entries, const Grid& grid, std::int64_t now_ns) noexcept;

/// One pass at @p now_ns (see TaskScheduler). @return the number of runs.
std::size_t run_pass(Span<Entry> entries, Grid& grid, std::int64_t now_ns, TaskClock clock, const void* runner) noexcept;

/// @return the earliest next activation of a periodic task that is not suspended, or kNever.
[[nodiscard]] std::int64_t next_activation_ns(Span<const Entry> entries, const Grid& grid) noexcept;

}  // namespace task_scheduler
}  // namespace detail

/// A cyclic executive for up to @p MaxTasks tasks (as the rate groups of OSEK/AUTOSAR): periodic
/// tasks on a time grid, and tasks run when triggered, all on one thread, highest priority first.
///
/// Running it: start(config) gives it a thread of its own that sleeps until the next activation (or
/// at most trigger_latency, to see triggers), runs a pass and beats heartbeat(); stop() ends it. Or
/// drive it from a loop of your own (or a test): align(origin) once, then run_once(now) every cycle;
/// such a loop passes the same origin to PeriodicTimer::start() and align(), with periods and offsets
/// that are multiples of the timer's period.
///
/// A pass at now: (1) the suspension requests of Task::suspend()/resume() are applied (a resumed task
/// rejoins its grid at the next point >= now, without catching up) and pending triggers are taken;
/// (2) the tasks run in priority order: a periodic task whose activation is due, and a triggered one,
/// once each per pass. A task suspended by an earlier task of the same pass does not run in it;
/// triggers and resumes made during a pass take effect at a later pass, and a trigger made while the
/// task itself runs gives one more run then.
///
/// Activations, in nanoseconds with saturating arithmetic: a periodic task keeps `next`, its first grid
/// point not yet released. A pass with now >= next releases the latest overdue grid point,
/// release = next + k * period with k = (now - next) / period, counts the k points skipped as missed
/// (TaskActivation::missed, TaskStatistics::missed_activations) and sets next = release + period; its
/// deadline is release + deadline (or + period). Unlike PeriodicTimer, the latest overdue point is run
/// late rather than skipped. A triggered run that is not also a periodic activation has release = now,
/// no missed points, and the deadline now + deadline (none if zero). Activations before add() or while
/// suspended are not missed.
///
/// Priority orders the tasks due in one pass; it never preempts. Rate groups that must preempt each
/// other (1 ms over 100 ms) use one TaskScheduler per thread, with rate-monotonic ThreadConfig
/// priorities.
///
/// Calls from a task's run(): Task::trigger(), suspend(), resume() and cancel() of any task; add(),
/// remove(), align(), run_once(), start() and stop() of this scheduler are contract violations
/// (COMMON_ASSERT).
///
/// RT-path safe: run_once(), next_activation(), size(), heartbeat() and the task requests. add(),
/// remove(), start(), stop() and align() belong to one control thread (initialisation phase), and
/// add() and remove() only while the scheduler's thread is not running. Give it static storage
/// duration. Declare it after its tasks: its destructor returns the tasks still in it to kIdle.
///
/// @code
///   static ControlTask g_control{};                  // 10 ms
///   static DiagnosticsTask g_diagnostics{};          // 100 ms, 5 ms after the control task
///   static common::TaskScheduler<8U> g_scheduler{};
///   static_cast<void>(g_scheduler.add(g_control, common::TaskSchedule::periodic(10ms)));
///   static_cast<void>(g_scheduler.add(g_diagnostics, common::TaskSchedule::periodic(100ms, 5ms)));
///   common::ThreadConfig config{};
///   config.name = "rt_tasks";
///   config.priority = 80;
///   const common::Status status{g_scheduler.start(config)};
///   ...
///   static_cast<void>(g_scheduler.stop());
/// @endcode
template <std::size_t MaxTasks>
class TaskScheduler final {
    static_assert(MaxTasks > 0U, "MaxTasks must be positive");

public:
    /// @param clock the time source: &MonotonicClock::now, or a test's fake clock.
    explicit TaskScheduler(const TaskClock clock = &MonotonicClock::now) noexcept : clock_{clock} {
        heartbeat_.beat(TimePoint::max());  // Parked until the thread runs.
    }

    /// @pre !running(). The tasks still in the scheduler become kIdle.
    ~TaskScheduler() {
        COMMON_ASSERT(!running_);
        for (std::size_t index{0U}; index < size_; ++index) {
            detail::TaskAccess::set_state(*entries_[index].task, TaskState::kIdle);
        }
    }

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    /// Adds @p task with @p schedule: claims it (kIdle or finished becomes kScheduled, or kSuspended),
    /// then drops its pending trigger and cancel requests. Added after an alignment, a periodic task
    /// joins the grid at its next point at or after the time of the last pass.
    /// @pre !running(); not called from a task's run()
    /// @return kOk; kInvalidArgument (see TaskSchedule); kAlreadyExists if the task is in a runner;
    ///         kResourceLimit when MaxTasks tasks are added.
    Status add(Task& task, const TaskSchedule& schedule) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        COMMON_ASSERT(!running_);
        Status status{detail::task_scheduler::validate(schedule)};
        if (is_ok(status) && (size_ >= MaxTasks)) {
            status = Status::kResourceLimit;
        }
        if (is_ok(status)) {
            TaskState previous{TaskState::kIdle};
            status = detail::TaskAccess::claim(task, schedule.suspended ? TaskState::kSuspended : TaskState::kScheduled,
                                               previous);
            if (is_ok(status)) {
                detail::TaskAccess::clear_trigger(task);
                detail::TaskAccess::request_suspension(task, schedule.suspended);
                detail::task_scheduler::Entry entry{&task, schedule, detail::task_scheduler::kNever, false};
                if (grid_.aligned && (schedule.period > Duration::zero())) {
                    entry.next_ns = detail::task_scheduler::first_point_at_or_after(schedule, grid_.origin_ns,
                                                                                    grid_.last_ns);
                }
                insert(entry);
            }
        }
        return status;
    }

    /// Removes @p task; it becomes kIdle.
    /// @pre !running(); not called from a task's run()
    /// @return kOk, or kNotFound if the task is not in this scheduler.
    Status remove(Task& task) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        COMMON_ASSERT(!running_);
        Status status{Status::kNotFound};
        std::size_t found{size_};
        for (std::size_t index{0U}; (index < size_) && (found == size_); ++index) {
            if (entries_[index].task == &task) {
                found = index;
            }
        }
        if (found < size_) {
            detail::TaskAccess::set_state(task, TaskState::kIdle);
            for (std::size_t index{found}; (index + 1U) < size_; ++index) {
                entries_[index] = entries_[index + 1U];
            }
            --size_;
            status = Status::kOk;
        }
        return status;
    }

    /// Starts the scheduler's own thread with @p config: it aligns the grid at the current time and then,
    /// until stop(), sleeps until the next activation (at most @p trigger_latency, the longest a
    /// trigger waits), runs a pass and beats heartbeat(). Allowed again after stop().
    /// @return kOk; kAlreadyRunning; kInvalidArgument if trigger_latency <= 0; or an RtThread::start() error.
    Status start(const ThreadConfig& config, const Duration trigger_latency = std::chrono::milliseconds{1}) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        Status status{Status::kAlreadyRunning};
        if (running_) {
            // kAlreadyRunning.
        } else if (trigger_latency <= Duration::zero()) {
            status = Status::kInvalidArgument;
        } else {
            trigger_latency_ = trigger_latency;
            exit_.store(false, std::memory_order_relaxed);
            status = thread_.start(config, &thread_entry, this);
            running_ = is_ok(status);
        }
        return status;
    }

    /// Non-RT: ends the thread after its current pass and joins it, then applies pending suspension
    /// requests. @pre not called from a task's run()
    /// @return kOk, kNotRunning, or an RtThread::join() error.
    Status stop() noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        Status status{Status::kNotRunning};
        if (running_) {
            exit_.store(true, std::memory_order_release);
            status = thread_.join();
            running_ = false;
            detail::task_scheduler::apply_suspensions(entries(), grid_, grid_.last_ns);
            heartbeat_.beat(TimePoint::max());
        }
        return status;
    }

    /// @return true from a successful start() until stop().
    [[nodiscard]] bool running() const noexcept {
        return running_;
    }

    /// Beaten by the scheduler's thread after every pass; parked (TimePoint::max()) while it is not running.
    [[nodiscard]] const Heartbeat& heartbeat() const noexcept {
        return heartbeat_;
    }

    /// For a loop of your own: sets the grid origin (repeating it realigns, keeps statistics and triggers).
    /// @pre !running(); not called from a task's run()
    void align(const TimePoint origin) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        COMMON_ASSERT(!running_);
        detail::task_scheduler::align(entries(), grid_, origin.time_since_epoch().count());
    }

    /// For a loop of your own: one pass at @p now (see the class).
    /// @pre !running(); align() was called; not called from a task's run()
    /// @return the number of runs (at most MaxTasks).
    std::size_t run_once(const TimePoint now) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        COMMON_ASSERT(!running_);
        COMMON_ASSERT(grid_.aligned);
        return detail::task_scheduler::run_pass(entries(), grid_, now.time_since_epoch().count(), clock_, this);
    }

    /// @return the earliest activation of a periodic task that is not suspended; TimePoint::max() if none.
    [[nodiscard]] TimePoint next_activation() const noexcept {
        return TimePoint{Duration{detail::task_scheduler::next_activation_ns(
            Span<const detail::task_scheduler::Entry>{entries_.data(), size_}, grid_)}};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return MaxTasks;
    }

private:
    [[nodiscard]] Span<detail::task_scheduler::Entry> entries() noexcept {
        return Span<detail::task_scheduler::Entry>{entries_.data(), size_};
    }

    /// Inserts after the entries of higher or equal priority (priority order, ties in add() order).
    void insert(const detail::task_scheduler::Entry& entry) noexcept {
        std::size_t position{0U};
        while ((position < size_) && (entries_[position].schedule.priority >= entry.schedule.priority)) {
            ++position;
        }
        for (std::size_t index{size_}; index > position; --index) {
            entries_[index] = entries_[index - 1U];
        }
        entries_[position] = entry;
        ++size_;
    }

    static void thread_entry(void* const context) noexcept {
        static_cast<TaskScheduler*>(context)->service_loop();
    }

    /// The thread's service loop: runs until stop() asks it to end.
    void service_loop() noexcept {
        detail::task_scheduler::align(entries(), grid_, clock_().time_since_epoch().count());
        while (!exit_.load(std::memory_order_acquire)) {
            const std::int64_t now_ns{clock_().time_since_epoch().count()};
            const std::int64_t next_ns{detail::task_scheduler::next_activation_ns(
                Span<const detail::task_scheduler::Entry>{entries_.data(), size_}, grid_)};
            const std::int64_t until_next_ns{saturating_sub(next_ns, now_ns)};
            const std::int64_t wait_ns{(until_next_ns < trigger_latency_.count()) ? until_next_ns
                                                                                   : trigger_latency_.count()};
            if (wait_ns > 0) {
                static_cast<void>(sleep_for(Duration{wait_ns}));
            }
            static_cast<void>(detail::task_scheduler::run_pass(entries(), grid_,
                                                               clock_().time_since_epoch().count(), clock_, this));
            heartbeat_.beat(MonotonicClock::now());
        }
    }

    std::array<detail::task_scheduler::Entry, MaxTasks> entries_{};
    std::size_t size_{0U};
    detail::task_scheduler::Grid grid_{};
    TaskClock clock_;
    Duration trigger_latency_{std::chrono::milliseconds{1}};
    std::atomic<bool> exit_{false};
    bool running_{false};
    RtThread thread_{};
    Heartbeat heartbeat_{};
};

}  // namespace common

#endif  // COMMON_TASK_TASK_SCHEDULER_HPP
