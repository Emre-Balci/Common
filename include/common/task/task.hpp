/// @file task.hpp
/// @brief The Task interface shared by TaskScheduler (periodic and triggered) and TaskExecutor (one-shot jobs).

#ifndef COMMON_TASK_TASK_HPP
#define COMMON_TASK_TASK_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <type_traits>

#include "common/concurrency/seqlock.hpp"
#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"

namespace common {

/// Where a task is in its life. Only its runner (a TaskScheduler or a TaskExecutor) changes it.
enum class TaskState : std::uint8_t {
    kIdle,       ///< In no runner: the initial state, and after TaskScheduler::remove().
    kScheduled,  ///< In a TaskScheduler, waiting for its next activation or a trigger.
    kSuspended,  ///< In a TaskScheduler, not activated until it is resumed.
    kQueued,     ///< Submitted to a TaskExecutor, waiting for a worker.
    kRunning,    ///< run() is executing.
    kDone,       ///< A job that finished: run() returned kOk.
    kFailed,     ///< A job that finished: run() returned an error.
    kCancelled,  ///< A job cancelled before it ran, or that returned an error after a cancel request.
};

[[nodiscard]] constexpr const char* to_string(const TaskState state) noexcept {
    const char* text{"unknown"};
    switch (state) {
        case TaskState::kIdle:
            text = "idle";
            break;
        case TaskState::kScheduled:
            text = "scheduled";
            break;
        case TaskState::kSuspended:
            text = "suspended";
            break;
        case TaskState::kQueued:
            text = "queued";
            break;
        case TaskState::kRunning:
            text = "running";
            break;
        case TaskState::kDone:
            text = "done";
            break;
        case TaskState::kFailed:
            text = "failed";
            break;
        case TaskState::kCancelled:
            text = "cancelled";
            break;
    }
    return text;
}

/// @return true for the final states of a job: kDone, kFailed and kCancelled.
[[nodiscard]] constexpr bool is_finished(const TaskState state) noexcept {
    return (state == TaskState::kDone) || (state == TaskState::kFailed) || (state == TaskState::kCancelled);
}

/// How a runner's stop() treats queued work (TaskExecutor, ActiveObject).
enum class StopMode : std::uint8_t {
    kDrain,   ///< Refuse new work, run everything already accepted, then the threads exit.
    kCancel,  ///< Refuse new work; queued jobs become kCancelled (queued commands are discarded); the
              ///< threads exit after the item they are running.
};

/// The clock of a runner: &MonotonicClock::now, or a test's fake clock. The now given to a runner's
/// functions comes from the same clock.
using TaskClock = TimePoint (*)() noexcept;

/// What a runner tells run() about one activation.
struct TaskActivation final {
    TimePoint now;           ///< When this run started: the runner's clock, read just before run().
    TimePoint release;       ///< When it became due: its activation time (TaskScheduler) or its submission (TaskExecutor).
    Deadline deadline;       ///< When it should be finished; Deadline::never() if there is no deadline.
    std::uint64_t sequence;  ///< Number of earlier runs of this task.
    std::uint64_t missed;    ///< Periodic activations skipped just before this run (0 for triggered runs and jobs).
};

/// Execution statistics of a task, in nanoseconds of its runner's clock.
///
/// A trivial aggregate without default member initialisers, because it is published through a
/// SeqLock (GCC 9's -Wclass-memaccess rejects SeqLock's memcpy into a type with a non-trivial default
/// constructor): value-initialise it, TaskStatistics statistics{}, for all counters 0 and last_status
/// kOk.
struct TaskStatistics final {
    std::uint64_t runs;                ///< Completed runs.
    std::uint64_t failures;            ///< Runs that returned an error.
    std::uint64_t overruns;            ///< Runs longer than their budget.
    std::uint64_t deadline_misses;     ///< Runs that finished after their deadline.
    std::uint64_t missed_activations;  ///< Periodic activations skipped because the task was late.
    std::int64_t last_execution_ns;    ///< Execution time of the last run.
    std::int64_t min_execution_ns;     ///< Shortest execution time; 0 before the first run.
    std::int64_t max_execution_ns;     ///< Longest execution time.
    std::int64_t total_execution_ns;   ///< Sum of the execution times (saturating): mean = total / runs.
    std::int64_t max_lateness_ns;      ///< Longest delay from release to start (never negative).
    Status last_status;                ///< What the last run returned.
};

static_assert(std::is_trivial<TaskStatistics>::value, "TaskStatistics is published through a SeqLock");

class Task;

namespace detail {

class TaskAccess;

/// Passkey that only TaskAccess can create: it gives the runners access to a task's internals
/// without a friend declaration in Task (a friend in a polymorphic class with a protected
/// destructor makes GCC's -Wnon-virtual-dtor fail). The explicit constructor keeps it from being an
/// aggregate, so TaskKey{} does not compile elsewhere.
class TaskKey final {
    friend class TaskAccess;
    explicit TaskKey() noexcept = default;
};

/// A task's runner-side state. Only the runners touch it, through TaskAccess.
struct TaskControl final {
    TaskControl() noexcept = default;
    ~TaskControl() = default;
    TaskControl(const TaskControl&) = delete;
    TaskControl(TaskControl&&) = delete;
    TaskControl& operator=(const TaskControl&) = delete;
    TaskControl& operator=(TaskControl&&) = delete;

    /// The state (the low bits, a TaskState) and the cancel request (kCancelBit) in one atomic, so
    /// that a claim clears a stale cancel request in the same compare-exchange and a cancel() made
    /// after the claim is never lost.
    std::atomic<std::uint8_t> word{0U};
    std::atomic<bool> trigger{false};             ///< Set by trigger(), taken by the scheduler.
    std::atomic<bool> suspend_requested{false};   ///< Set by suspend()/resume(), applied by the scheduler.
    std::int64_t release_ns{0};                   ///< Submission time of a job; written by the claimer only.
    Deadline deadline{};                          ///< Deadline of a job; written by the claimer only.
    TaskStatistics statistics{};                  ///< The runner's copy; written by the runner's thread only.
    SeqLock<TaskStatistics> published{};          ///< Snapshot for other threads.
};

}  // namespace detail

/// Something a runner executes: periodically or when triggered (TaskScheduler), or once (TaskExecutor).
/// A class derives from Task and implements run(): one activation of bounded work, on the runner's
/// thread, which never runs the same task concurrently.
///
/// - state() tells where the task is (TaskState). Only the runner writes it: a task belongs to its
///   runner from TaskScheduler::add()/TaskExecutor::submit() until it is removed or finished, and may
///   be destroyed only when it is in no runner (kIdle or finished; asserted).
/// - cancel(), trigger(), suspend() and resume() are requests, wait-free from any thread; they never
///   change state() themselves. A TaskExecutor honours cancel() (a queued job is not run; a running
///   one sees cancel_requested() and may return early); a TaskScheduler honours trigger(),
///   suspend() and resume() and ignores cancel(). Several triggers before the task runs count once.
/// - Statistics are kept by the runner; try_statistics() reads a consistent snapshot from any thread.
///
/// A leaf class is final; an intermediate class declares a protected non-virtual destructor and no
/// friends (otherwise -Wnon-virtual-dtor or MSVC's C4265 fails the build). A unit owns its tasks as
/// members (deriving from both Unit and Task would make name() ambiguous).
///
/// RT-path safe: state(), try_statistics() and the requests.
///
/// @code
///   class ControlTask final : public common::Task {
///   public:
///       ControlTask() noexcept : Task{"control"} {}
///   private:
///       common::Status run(const common::TaskActivation& activation) noexcept override {
///           // One control step; activation.missed tells about skipped periods.
///           return common::Status::kOk;
///       }
///   };
/// @endcode
class Task {
public:
    [[nodiscard]] const char* name() const noexcept {
        return name_;
    }

    /// Any thread.
    [[nodiscard]] TaskState state() const noexcept;

    /// Any thread: a consistent snapshot of the statistics.
    /// @return false if every attempt overlapped an update (@p out is left untouched).
    [[nodiscard]] bool try_statistics(TaskStatistics& out) const noexcept {
        return control_.published.try_load(out);
    }

    /// Any thread: requests cancellation (see the class).
    void cancel() noexcept;

    /// Any thread. run() polls it to stop early.
    [[nodiscard]] bool cancel_requested() const noexcept;

    /// Any thread: asks the TaskScheduler that has this task for one more run at its next pass.
    void trigger() noexcept {
        static_cast<void>(control_.trigger.exchange(true, std::memory_order_release));
    }

    /// Any thread: asks the TaskScheduler that has this task to stop activating it from its next pass on.
    void suspend() noexcept {
        control_.suspend_requested.store(true, std::memory_order_release);
    }

    /// Any thread: asks the TaskScheduler that has this task to activate it again from its next pass on,
    /// at its next grid point (no catch-up).
    void resume() noexcept {
        control_.suspend_requested.store(false, std::memory_order_release);
    }

    /// For the runners only (they hold the key): the task's runner-side state.
    [[nodiscard]] detail::TaskControl& control(const detail::TaskKey /*key*/) noexcept {
        return control_;
    }

    /// For the runners only (they hold the key): runs one activation.
    [[nodiscard]] Status run_activation(const detail::TaskKey /*key*/, const TaskActivation& activation) noexcept {
        return run(activation);
    }

protected:
    /// @param name shown in diagnostics; static storage duration; nullptr becomes "".
    explicit Task(const char* name) noexcept;

    /// @pre the task is in no runner: state() is kIdle or finished.
    ~Task();

    Task(const Task&) = delete;
    Task(Task&&) = delete;
    Task& operator=(const Task&) = delete;
    Task& operator=(Task&&) = delete;

    /// One activation: bounded work on the runner's thread. Its Status is counted in the statistics and
    /// decides a job's final state.
    virtual Status run(const TaskActivation& activation) noexcept = 0;

private:
    const char* name_;
    detail::TaskControl control_{};
};

/// Waits until is_finished(task.state()), sleeping in steps of @p poll (non-RT: it blocks), at most
/// until @p deadline (Deadline::never() waits indefinitely).
/// @return kOk once finished, kTimeout, kInvalidArgument if poll <= 0, or the error of a sleep.
Status wait_until_finished(const Task& task, const Deadline& deadline,
                           Duration poll = std::chrono::milliseconds{1}) noexcept;

/// Waits until task.state() == @p state, like wait_until_finished(); e.g. for a suspension requested
/// with suspend() to take effect.
Status wait_until_state(const Task& task, TaskState state, const Deadline& deadline,
                        Duration poll = std::chrono::milliseconds{1}) noexcept;

namespace detail {

/// The runners' access to a task (TaskScheduler, TaskExecutor). Not for applications.
class TaskAccess final {
public:
    TaskAccess() = delete;

    /// Claims an idle or finished task for a runner: compare-exchange of its state from kIdle, kDone,
    /// kFailed or kCancelled to @p desired (acquire-release, so the previous runner's writes come
    /// before this runner's), clearing a stale cancel request. @p previous receives the state it had.
    /// @return kOk; kAlreadyExists if the task is in a runner; kResourceLimit if too many concurrent
    ///         changes kept the exchange from succeeding (bounded attempts).
    [[nodiscard]] static Status claim(Task& task, TaskState desired, TaskState& previous) noexcept;

    /// Sets the state (release), keeping the cancel request.
    static void set_state(Task& task, TaskState state) noexcept;

    /// Takes the trigger request (acquire). @return true if there was one.
    [[nodiscard]] static bool take_trigger(Task& task) noexcept;

    /// Drops a pending trigger request.
    static void clear_trigger(Task& task) noexcept;

    /// The suspension last requested with suspend()/resume().
    [[nodiscard]] static bool suspension_requested(Task& task) noexcept;

    /// Sets the suspension request (TaskScheduler::add() with a schedule that starts suspended).
    static void request_suspension(Task& task, bool suspended) noexcept;

    /// Records a job's submission time and deadline (by the claimer, after a successful claim).
    static void set_submission(Task& task, TimePoint release, const Deadline& deadline) noexcept;

    [[nodiscard]] static TimePoint submission_release(Task& task) noexcept;
    [[nodiscard]] static Deadline submission_deadline(Task& task) noexcept;

    /// Number of completed runs (the runner's copy of the statistics).
    [[nodiscard]] static std::uint64_t runs(Task& task) noexcept;

    /// Runs one activation on the calling thread: calls run(), measures its execution time with
    /// @p clock, updates the statistics (overrun if @p budget > 0 and the run took longer; deadline
    /// miss if it finished after activation.deadline; lateness = now - release, at least 0;
    /// missed_activations += activation.missed) and publishes them before returning, so that they are
    /// visible to a reader that sees the state the caller stores next. Does not change the state.
    /// @return what run() returned.
    [[nodiscard]] static Status execute(Task& task, const TaskActivation& activation, Duration budget,
                                        TaskClock clock) noexcept;

private:
    [[nodiscard]] static TaskKey key() noexcept {
        return TaskKey{};
    }
};

/// Names the runner whose thread or pass is executing user code on this thread, for the runners'
/// re-entry checks (e.g. a job that calls stop() of its own executor would join its own thread).
/// Restores the previous runner when it ends, so scopes nest.
class RunnerScope final {
public:
    explicit RunnerScope(const void* runner) noexcept;
    ~RunnerScope();
    RunnerScope(const RunnerScope&) = delete;
    RunnerScope(RunnerScope&&) = delete;
    RunnerScope& operator=(const RunnerScope&) = delete;
    RunnerScope& operator=(RunnerScope&&) = delete;

private:
    const void* previous_;
};

/// The runner of the innermost RunnerScope on this thread, or nullptr.
[[nodiscard]] const void* current_runner() noexcept;

}  // namespace detail

}  // namespace common

#endif  // COMMON_TASK_TASK_HPP
