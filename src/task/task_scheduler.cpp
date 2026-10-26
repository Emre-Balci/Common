#include "common/task/task_scheduler.hpp"

#include <cstddef>
#include <cstdint>

#include "common/numeric/integer.hpp"

namespace common {
namespace detail {
namespace task_scheduler {

namespace {

[[nodiscard]] bool is_periodic(const TaskSchedule& schedule) noexcept {
    return schedule.period > Duration::zero();
}

[[nodiscard]] TimePoint time_point_of(const std::int64_t ns) noexcept {
    return TimePoint{Duration{ns}};
}

/// Applies the suspension request of one entry at now.
void apply_suspension(Entry& entry, const Grid& grid, const std::int64_t now_ns) noexcept {
    Task& task{*entry.task};
    const bool requested{TaskAccess::suspension_requested(task)};
    const TaskState state{task.state()};
    if (requested && (state == TaskState::kScheduled)) {
        TaskAccess::set_state(task, TaskState::kSuspended);
        TaskAccess::clear_trigger(task);
        entry.triggered = false;
    } else if ((!requested) && (state == TaskState::kSuspended)) {
        // Triggers made while suspended are dropped; the grid resumes at its next point, without the
        // activations of the suspended time.
        TaskAccess::clear_trigger(task);
        entry.triggered = false;
        if (is_periodic(entry.schedule) && grid.aligned) {
            entry.next_ns = first_point_at_or_after(entry.schedule, grid.origin_ns, now_ns);
        }
        TaskAccess::set_state(task, TaskState::kScheduled);
    } else {
        // No change.
    }
}

/// Runs the entry's task if it is due at now (periodically, or by a trigger taken at the start of the
/// pass). @return true if it ran.
bool run_if_due(Entry& entry, const Grid& grid, const std::int64_t now_ns, const TaskClock clock) noexcept {
    const TaskSchedule& schedule{entry.schedule};
    const std::int64_t period_ns{schedule.period.count()};
    const bool due{is_periodic(schedule) && grid.aligned && (entry.next_ns != kNever) && (now_ns >= entry.next_ns)};
    bool ran{false};
    if (due || entry.triggered) {
        std::int64_t release_ns{now_ns};
        std::uint64_t missed{0U};
        Deadline deadline{Deadline::never()};
        if (due) {
            // The latest overdue grid point is released; the k points before it are missed.
            const std::int64_t k{saturating_sub(now_ns, entry.next_ns) / period_ns};
            release_ns = saturating_add(entry.next_ns, saturating_mul(k, period_ns));
            missed = static_cast<std::uint64_t>(k);
            entry.next_ns = saturating_add(release_ns, period_ns);
            const std::int64_t relative_ns{(schedule.deadline > Duration::zero()) ? schedule.deadline.count() : period_ns};
            deadline = Deadline::at(time_point_of(saturating_add(release_ns, relative_ns)));
        } else if (schedule.deadline > Duration::zero()) {
            deadline = Deadline::at(time_point_of(saturating_add(now_ns, schedule.deadline.count())));
        } else {
            // A triggered run without a deadline.
        }
        entry.triggered = false;
        Task& task{*entry.task};
        TaskAccess::set_state(task, TaskState::kRunning);
        const TaskActivation activation{clock(), time_point_of(release_ns), deadline, TaskAccess::runs(task), missed};
        static_cast<void>(TaskAccess::execute(task, activation, schedule.budget, clock));
        if (TaskAccess::suspension_requested(task)) {
            // Suspended during its own run: the suspension takes effect now.
            TaskAccess::clear_trigger(task);
            TaskAccess::set_state(task, TaskState::kSuspended);
        } else {
            TaskAccess::set_state(task, TaskState::kScheduled);
        }
        ran = true;
    }
    return ran;
}

}  // namespace

Status validate(const TaskSchedule& schedule) noexcept {
    Status status{Status::kOk};
    if ((schedule.period < Duration::zero()) || (schedule.offset < Duration::zero()) ||
        (schedule.budget < Duration::zero()) || (schedule.deadline < Duration::zero())) {
        status = Status::kInvalidArgument;
    } else if (schedule.triggered_only == is_periodic(schedule)) {
        // Exactly one of: a period, or triggered_only.
        status = Status::kInvalidArgument;
    } else {
        // Valid.
    }
    return status;
}

std::int64_t first_point_at_or_after(const TaskSchedule& schedule, const std::int64_t origin_ns,
                                     const std::int64_t time_ns) noexcept {
    const std::int64_t base_ns{saturating_add(origin_ns, schedule.offset.count())};
    std::int64_t point_ns{base_ns};
    const std::int64_t period_ns{schedule.period.count()};
    if ((time_ns > base_ns) && (period_ns > 0)) {
        const std::int64_t delta_ns{saturating_sub(time_ns, base_ns)};
        std::int64_t periods{delta_ns / period_ns};
        if ((delta_ns % period_ns) != 0) {
            ++periods;
        }
        point_ns = saturating_add(base_ns, saturating_mul(periods, period_ns));
    }
    return point_ns;
}

void align(const Span<Entry> entries, Grid& grid, const std::int64_t origin_ns) noexcept {
    grid.origin_ns = origin_ns;
    grid.last_ns = origin_ns;
    grid.aligned = true;
    for (Entry& entry : entries) {
        entry.next_ns = is_periodic(entry.schedule) ? saturating_add(origin_ns, entry.schedule.offset.count()) : kNever;
    }
}

void apply_suspensions(const Span<Entry> entries, const Grid& grid, const std::int64_t now_ns) noexcept {
    for (Entry& entry : entries) {
        apply_suspension(entry, grid, now_ns);
    }
}

std::size_t run_pass(const Span<Entry> entries, Grid& grid, const std::int64_t now_ns, const TaskClock clock,
                     const void* const runner) noexcept {
    const RunnerScope scope{runner};
    grid.last_ns = now_ns;
    // (1) Suspension requests, and the triggers of this pass: later ones wait for the next pass.
    for (Entry& entry : entries) {
        apply_suspension(entry, grid, now_ns);
        entry.triggered = (entry.task->state() == TaskState::kScheduled) && TaskAccess::take_trigger(*entry.task);
    }
    // (2) The runs, in priority order.
    std::size_t runs{0U};
    for (Entry& entry : entries) {
        Task& task{*entry.task};
        if ((task.state() == TaskState::kScheduled) && TaskAccess::suspension_requested(task)) {
            // Suspended by an earlier task of this pass: it does not run.
            TaskAccess::clear_trigger(task);
            entry.triggered = false;
            TaskAccess::set_state(task, TaskState::kSuspended);
        }
        if ((task.state() == TaskState::kScheduled) && run_if_due(entry, grid, now_ns, clock)) {
            ++runs;
        }
    }
    return runs;
}

std::int64_t next_activation_ns(const Span<const Entry> entries, const Grid& grid) noexcept {
    std::int64_t next_ns{kNever};
    if (grid.aligned) {
        for (const Entry& entry : entries) {
            if (is_periodic(entry.schedule) && (entry.task->state() != TaskState::kSuspended) &&
                (entry.next_ns < next_ns)) {
                next_ns = entry.next_ns;
            }
        }
    }
    return next_ns;
}

}  // namespace task_scheduler
}  // namespace detail
}  // namespace common
