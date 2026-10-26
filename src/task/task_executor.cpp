#include "common/task/task_executor.hpp"

namespace common {
namespace detail {
namespace task_executor {

Status prepare(Task& task, TaskState& previous, const TimePoint release, const Deadline& deadline) noexcept {
    const Status status{TaskAccess::claim(task, TaskState::kQueued, previous)};
    if (is_ok(status)) {
        // Only the claimer writes these; the queue's release and acquire hand them to the worker.
        TaskAccess::set_submission(task, release, deadline);
    }
    return status;
}

void restore(Task& task, const TaskState previous) noexcept {
    TaskAccess::set_state(task, previous);
}

void process(Task& task, const TaskClock clock, const void* const runner) noexcept {
    if (task.cancel_requested()) {
        TaskAccess::set_state(task, TaskState::kCancelled);
    } else {
        TaskAccess::set_state(task, TaskState::kRunning);
        Status status{Status::kOk};
        {
            const RunnerScope scope{runner};
            const TaskActivation activation{clock(), TaskAccess::submission_release(task),
                                            TaskAccess::submission_deadline(task), TaskAccess::runs(task), 0U};
            status = TaskAccess::execute(task, activation, Duration::zero(), clock);
        }
        TaskState final_state{TaskState::kDone};
        if (!is_ok(status)) {
            final_state = task.cancel_requested() ? TaskState::kCancelled : TaskState::kFailed;
        }
        // The last access: the submitter may reuse or destroy the task from now on.
        TaskAccess::set_state(task, final_state);
    }
}

void cancel(Task& task) noexcept {
    TaskAccess::set_state(task, TaskState::kCancelled);
}

}  // namespace task_executor
}  // namespace detail
}  // namespace common
