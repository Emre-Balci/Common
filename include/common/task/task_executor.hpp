/// @file task_executor.hpp
/// @brief A fixed pool of worker threads that runs one-shot jobs (Tasks) submitted from any thread.

#ifndef COMMON_TASK_TASK_EXECUTOR_HPP
#define COMMON_TASK_TASK_EXECUTOR_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "common/concurrency/mpmc_queue.hpp"
#include "common/concurrency/spin.hpp"
#include "common/core/assert.hpp"
#include "common/core/status.hpp"
#include "common/diagnostics/watchdog.hpp"
#include "common/system/thread.hpp"
#include "common/task/task.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"

namespace common {

namespace detail {
namespace task_executor {

/// Rounds of idle_sleep that stop() waits for submit() calls in flight before it gives up (kTimeout).
inline constexpr std::uint32_t kMaxStopWaitRounds{1000U};

/// The claim and bookkeeping of submit() before the push: kQueued, submission time and deadline.
/// @return kOk, or the error of TaskAccess::claim().
[[nodiscard]] Status prepare(Task& task, TaskState& previous, TimePoint release, const Deadline& deadline) noexcept;

/// Undoes prepare() after a failed push.
void restore(Task& task, TaskState previous) noexcept;

/// A dequeued job: runs it (kRunning, then kDone, kFailed or kCancelled), or makes it kCancelled
/// without running it when a cancel was requested. The final state store is the last access to the
/// task: its submitter may reuse or destroy it from then on.
void process(Task& task, TaskClock clock, const void* runner) noexcept;

/// A dequeued job that will not run: kCancelled.
void cancel(Task& task) noexcept;

}  // namespace task_executor
}  // namespace detail

/// Runs one-shot jobs on @p Workers worker threads: any thread submit()s a Task, a worker runs it once,
/// and its state tells the outcome (kDone, kFailed or kCancelled), like a std::packaged_task without
/// heap or blocking. For work that must leave a real-time thread: writing a snapshot, flushing a
/// journal, a slow computation.
///
/// - submit() is lock-free and bounded: it claims the task (kIdle or finished becomes kQueued) and
///   pushes it into a queue of @p QueueCapacity entries (an MpmcQueue). A task submitted twice, or
///   that is in a TaskScheduler, is refused (kAlreadyExists); a full queue, kResourceLimit.
/// - A submitted task belongs to the executor until its state is finished; then the submitter may read
///   its statistics, submit it again or destroy it. wait_until_finished() waits for it (non-RT).
/// - Task::cancel(): a queued job is not run (it becomes kCancelled when it is dequeued); a running job
///   sees cancel_requested() and may return early, and it then ends kCancelled if it returns an error.
/// - A job runs on a worker with the run's start time, its submission time as release, and the
///   deadline given to submit() (the statistics count a finish after it as a deadline miss).
/// - stop(kDrain) refuses new jobs, lets the workers run every job accepted, and joins them;
///   stop(kCancel) refuses new jobs, lets each worker finish the job it is running, joins them, and
///   cancels the jobs still queued. A running job is not interrupted: call its cancel() first if it
///   should return early. Without running workers, stop() cancels the jobs still queued. A submit()
///   that returned kOk is never lost: its job runs or becomes kCancelled. The executor can be started
///   again after stop().
/// - Each worker beats its heartbeat() every round, so a Watchdog can supervise it; the heartbeats are
///   parked (TimePoint::max()) while the workers are not running.
/// - Without workers (before start()), run_pending() runs queued jobs on the calling thread: for tests
///   and single-threaded programs.
///
/// Calls from a job: submit(), request_stop() and the task requests are allowed; start(), stop() and
/// run_pending() of the same executor are contract violations (COMMON_ASSERT): a worker cannot join
/// itself.
///
/// RT-path safe: submit(), request_stop(), stopping(), heartbeat(). run_pending(): as the jobs it runs.
/// start(), stop() and running() belong to one control thread (initialisation and shutdown). Workers
/// wait for jobs by sleeping idle_sleep, so a job submitted to idle workers starts after up to about
/// idle_sleep. Give it static storage duration, and declare it after the tasks it runs: its destructor
/// cancels the jobs still queued.
///
/// @code
///   static SnapshotJob g_snapshot{};
///   static common::TaskExecutor<2U, 64U> g_background{};
///   common::ThreadConfig config{};
///   config.name = "background";
///   config.policy = common::SchedPolicy::kOther;
///   config.priority = 0;
///   static_cast<void>(g_background.start(config));
///   // On a real-time thread: never blocks.
///   if (g_background.submit(g_snapshot) != common::Status::kOk) { ... }  // busy: try later
///   // At shutdown, after the job's producers have stopped:
///   static_cast<void>(g_background.stop(common::StopMode::kDrain));
/// @endcode
template <std::size_t Workers, std::size_t QueueCapacity>
class TaskExecutor final {
    static_assert(Workers > 0U, "Workers must be positive");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "std::atomic<std::uint32_t> must be lock-free");
    static_assert(std::atomic<std::size_t>::is_always_lock_free, "std::atomic<std::size_t> must be lock-free");

public:
    /// @param clock measures the jobs: &MonotonicClock::now, or a test's fake clock.
    explicit TaskExecutor(const TaskClock clock = &MonotonicClock::now) noexcept : clock_{clock} {
        for (std::size_t index{0U}; index < Workers; ++index) {
            workers_[index].owner = this;
            workers_[index].index = index;
            heartbeats_[index].beat(TimePoint::max());  // Parked until the workers run.
        }
    }

    /// @pre !running(). Cancels the jobs still queued.
    ~TaskExecutor() {
        COMMON_ASSERT(!running_);
        cancel_queued();
    }

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor(TaskExecutor&&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;
    TaskExecutor& operator=(TaskExecutor&&) = delete;

    /// Starts the Workers threads, each with @p config, sleeping @p idle_sleep when there is nothing to
    /// do. Allowed again after stop(); jobs submitted before start() are run.
    /// @pre not called from a job of this executor
    /// @return kOk; kAlreadyRunning; kInvalidArgument if idle_sleep <= 0; or an RtThread::start() error,
    ///         after the workers already started have been stopped again.
    Status start(const ThreadConfig& config, const Duration idle_sleep = std::chrono::milliseconds{1}) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        Status status{Status::kAlreadyRunning};
        if (running_) {
            // kAlreadyRunning.
        } else if (idle_sleep <= Duration::zero()) {
            status = Status::kInvalidArgument;
        } else {
            idle_sleep_ = idle_sleep;
            exit_.store(false, std::memory_order_relaxed);
            cancel_mode_.store(false, std::memory_order_relaxed);
            stopping_.store(false, std::memory_order_seq_cst);
            status = Status::kOk;
            std::size_t started{0U};
            for (std::size_t index{0U}; (index < Workers) && is_ok(status); ++index) {
                status = threads_[index].start(config, &worker_entry, &workers_[index]);
                if (is_ok(status)) {
                    ++started;
                }
            }
            if (is_ok(status)) {
                running_ = true;
            } else {
                // Stop the workers already started; the queue keeps its jobs.
                cancel_mode_.store(true, std::memory_order_relaxed);
                exit_.store(true, std::memory_order_release);
                static_cast<void>(join_workers(started));
                exit_.store(false, std::memory_order_relaxed);
            }
        }
        return status;
    }

    /// Any thread, lock-free: queues @p task as a job whose deadline is @p deadline. Allowed before
    /// start() (run_pending() can run it).
    /// @return kOk; kAlreadyExists if the task is in a runner (queued, running, or in a TaskScheduler);
    ///         kResourceLimit if the queue is full or too contended (the task is left as it was, but a
    ///         pending cancel request is dropped); kNotRunning after stop() or request_stop().
    Status submit(Task& task, const Deadline& deadline = Deadline::never()) noexcept {
        // The in-flight count and the stopping flag form a handshake with stop() (both seq_cst): a
        // submit either sees stopping_, or is counted and finishes before stop() goes on.
        static_cast<void>(in_flight_.fetch_add(1U, std::memory_order_seq_cst));
        Status status{Status::kNotRunning};
        if (!stopping_.load(std::memory_order_seq_cst)) {
            TaskState previous{TaskState::kIdle};
            status = detail::task_executor::prepare(task, previous, clock_(), deadline);
            if (is_ok(status)) {
                // Counted before the push, so that queued_ is never below the number of queued jobs.
                static_cast<void>(queued_.fetch_add(1U, std::memory_order_relaxed));
                if (!queue_.try_push(&task)) {
                    static_cast<void>(queued_.fetch_sub(1U, std::memory_order_relaxed));
                    detail::task_executor::restore(task, previous);
                    status = Status::kResourceLimit;
                }
            }
        }
        static_cast<void>(in_flight_.fetch_sub(1U, std::memory_order_seq_cst));
        return status;
    }

    /// Runs up to @p max_jobs queued jobs on the calling thread.
    /// @pre not called from a job of this executor
    /// @return the number of jobs dequeued (run or cancelled).
    std::size_t run_pending(const std::size_t max_jobs) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        std::size_t dequeued{0U};
        Task* task{nullptr};
        while ((dequeued < max_jobs) && queue_.try_pop(task)) {
            static_cast<void>(queued_.fetch_sub(1U, std::memory_order_acq_rel));
            detail::task_executor::process(*task, clock_, this);
            ++dequeued;
        }
        return dequeued;
    }

    /// Non-RT: refuses new jobs, then stops the workers as @p mode says (see the class) and joins them,
    /// and cancels the jobs still queued.
    /// @pre not called from a job of this executor
    /// @return kOk; kNotRunning if no workers were running (queued jobs are still cancelled); kTimeout
    ///         if submit() calls were still in flight after the bounded wait (the executor stays
    ///         stopping: call stop() again); or an RtThread::join() error.
    Status stop(const StopMode mode = StopMode::kDrain) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        if (running_ && (mode == StopMode::kCancel)) {
            // Set before stopping_, so that whoever sees stopping() also sees that the workers exit.
            cancel_mode_.store(true, std::memory_order_relaxed);
            exit_.store(true, std::memory_order_release);
        }
        stopping_.store(true, std::memory_order_seq_cst);
        bool settled{in_flight_.load(std::memory_order_seq_cst) == 0U};
        for (std::uint32_t round{0U}; (round < detail::task_executor::kMaxStopWaitRounds) && (!settled); ++round) {
            static_cast<void>(sleep_for(idle_sleep_));
            settled = (in_flight_.load(std::memory_order_seq_cst) == 0U);
        }
        Status status{Status::kTimeout};
        if (settled) {
            status = Status::kNotRunning;
            if (running_) {
                // kDrain: the workers run the queue empty; with no submit in flight nothing is added.
                exit_.store(true, std::memory_order_release);
                status = join_workers(Workers);
                running_ = false;
            }
            cancel_queued();
        }
        return status;
    }

    /// Any thread, a job included: refuses new jobs from now on (submit() returns kNotRunning); the
    /// workers go on. stop() still has to be called to end them.
    void request_stop() noexcept {
        stopping_.store(true, std::memory_order_seq_cst);
    }

    /// Any thread: true from stop() or request_stop() until the next start().
    [[nodiscard]] bool stopping() const noexcept {
        return stopping_.load(std::memory_order_seq_cst);
    }

    /// @return true from a successful start() until stop().
    [[nodiscard]] bool running() const noexcept {
        return running_;
    }

    /// Beaten by worker @p worker every round; parked (TimePoint::max()) while the workers are not running.
    /// @pre worker < Workers
    [[nodiscard]] const Heartbeat& heartbeat(const std::size_t worker) const noexcept {
        COMMON_ASSERT(worker < Workers);
        return heartbeats_[worker];
    }

private:
    struct Worker final {
        TaskExecutor* owner{nullptr};
        std::size_t index{0U};
    };

    static void worker_entry(void* const context) noexcept {
        const Worker* const worker{static_cast<const Worker*>(context)};
        worker->owner->worker_loop(worker->index);
    }

    /// A worker's service loop: runs until stop() tells it to exit.
    void worker_loop(const std::size_t index) noexcept {
        bool active{true};
        while (active) {
            heartbeats_[index].beat(MonotonicClock::now());
            const bool exiting{exit_.load(std::memory_order_acquire)};
            Task* task{nullptr};
            if (exiting && cancel_mode_.load(std::memory_order_relaxed)) {
                active = false;
            } else if (queue_.try_pop(task)) {
                static_cast<void>(queued_.fetch_sub(1U, std::memory_order_acq_rel));
                detail::task_executor::process(*task, clock_, this);
            } else if (exiting && (queued_.load(std::memory_order_acquire) == 0U)) {
                active = false;  // Drained: no submit is in flight any more, so nothing else comes.
            } else if (exiting) {
                cpu_relax();  // A pop lost a race with another worker; jobs are left.
            } else {
                static_cast<void>(sleep_for(idle_sleep_));
            }
        }
        heartbeats_[index].beat(TimePoint::max());
    }

    /// Joins the first @p count workers. @return kOk or the first join error.
    Status join_workers(const std::size_t count) noexcept {
        Status status{Status::kOk};
        for (std::size_t index{0U}; index < count; ++index) {
            const Status joined{threads_[index].join()};
            if (is_ok(status)) {
                status = joined;
            }
        }
        return status;
    }

    /// Cancels the jobs still queued, with no worker running and no submit in flight.
    void cancel_queued() noexcept {
        Task* task{nullptr};
        for (std::size_t count{0U}; (count < QueueCapacity) && queue_.try_pop(task); ++count) {
            static_cast<void>(queued_.fetch_sub(1U, std::memory_order_acq_rel));
            detail::task_executor::cancel(*task);
        }
    }

    MpmcQueue<Task*, QueueCapacity> queue_{};
    std::atomic<std::uint32_t> in_flight_{0U};  // submit() calls between their two steps of the handshake.
    std::atomic<std::size_t> queued_{0U};       // Jobs pushed or being pushed, and not yet dequeued.
    std::atomic<bool> stopping_{false};
    std::atomic<bool> exit_{false};
    std::atomic<bool> cancel_mode_{false};
    std::array<Worker, Workers> workers_{};
    std::array<RtThread, Workers> threads_{};
    std::array<Heartbeat, Workers> heartbeats_{};
    Duration idle_sleep_{std::chrono::milliseconds{1}};
    TaskClock clock_;
    bool running_{false};
};

}  // namespace common

#endif  // COMMON_TASK_TASK_EXECUTOR_HPP
