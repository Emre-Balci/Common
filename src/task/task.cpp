#include "common/task/task.hpp"

#include <atomic>
#include <cstdint>
#include <limits>

#include "common/core/assert.hpp"
#include "common/numeric/integer.hpp"

namespace common {

namespace {

static_assert(std::atomic<std::uint8_t>::is_always_lock_free, "std::atomic<std::uint8_t> must be lock-free");
static_assert(std::atomic<bool>::is_always_lock_free, "std::atomic<bool> must be lock-free");

// The state word: a TaskState in the low bits, the cancel request in the top bit.
constexpr std::uint8_t kStateMask{0x0FU};
constexpr std::uint8_t kCancelBit{0x80U};

// A claim gives up after this many failed compare-exchanges (each failure means that another thread
// changed the word in between, e.g. with cancel()).
constexpr std::uint32_t kMaxClaimAttempts{64U};

// set_state() needs at most two strong compare-exchanges: only cancel() changes the word concurrently,
// and only once (setting the bit again changes nothing).
constexpr std::uint32_t kMaxSetStateAttempts{4U};

[[nodiscard]] constexpr TaskState state_of(const std::uint8_t word) noexcept {
    return static_cast<TaskState>(static_cast<std::uint8_t>(word & kStateMask));
}

[[nodiscard]] constexpr std::uint8_t bits_of(const TaskState state) noexcept {
    return static_cast<std::uint8_t>(state);
}

[[nodiscard]] constexpr bool is_claimable(const TaskState state) noexcept {
    return (state == TaskState::kIdle) || is_finished(state);
}

// The thread's innermost RunnerScope. Constant-initialised: no dynamic initialisation, no allocation.
thread_local const void* g_current_runner{nullptr};

[[nodiscard]] bool reached_state(const TaskState state, const TaskState wanted) noexcept {
    return state == wanted;
}

[[nodiscard]] bool reached_finished(const TaskState state, const TaskState /*wanted*/) noexcept {
    return is_finished(state);
}

using Reached = bool (*)(TaskState state, TaskState wanted) noexcept;

// Waits until reached(task.state(), wanted), sleeping at most poll at a time and never past the
// deadline. The number of rounds is bounded before the loop starts: remaining / poll + 1.
Status wait_until(const Task& task, const Reached reached, const TaskState wanted, const Deadline& deadline,
                  const Duration poll) noexcept {
    Status status{Status::kInvalidArgument};
    if (poll > Duration::zero()) {
        const std::int64_t remaining_ns{deadline.remaining().count()};
        const std::uint64_t rounds{(static_cast<std::uint64_t>(remaining_ns) / static_cast<std::uint64_t>(poll.count())) +
                                   1U};
        bool done{reached(task.state(), wanted)};
        bool failed{false};
        for (std::uint64_t round{0U}; (round < rounds) && (!done) && (!failed) && (!deadline.expired()); ++round) {
            const TimePoint now{MonotonicClock::now()};
            const std::int64_t step_end_ns{saturating_add(now.time_since_epoch().count(), poll.count())};
            const std::int64_t deadline_ns{deadline.time_point().time_since_epoch().count()};
            const TimePoint wake{Duration{(step_end_ns < deadline_ns) ? step_end_ns : deadline_ns}};
            const Status slept{sleep_until(wake)};
            if (is_ok(slept)) {
                done = reached(task.state(), wanted);
            } else {
                status = slept;
                failed = true;
            }
        }
        if (done) {
            status = Status::kOk;
        } else if (!failed) {
            status = Status::kTimeout;
        } else {
            // The sleep error is kept.
        }
    }
    return status;
}

}  // namespace

Task::Task(const char* const name) noexcept : name_{(name != nullptr) ? name : ""} {}

Task::~Task() {
    const TaskState current{state()};
    COMMON_ASSERT((current == TaskState::kIdle) || is_finished(current));
}

TaskState Task::state() const noexcept {
    return state_of(control_.word.load(std::memory_order_acquire));
}

void Task::cancel() noexcept {
    static_cast<void>(control_.word.fetch_or(kCancelBit, std::memory_order_acq_rel));
}

bool Task::cancel_requested() const noexcept {
    return (control_.word.load(std::memory_order_acquire) & kCancelBit) != 0U;
}

Status wait_until_finished(const Task& task, const Deadline& deadline, const Duration poll) noexcept {
    return wait_until(task, &reached_finished, TaskState::kDone, deadline, poll);
}

Status wait_until_state(const Task& task, const TaskState state, const Deadline& deadline,
                        const Duration poll) noexcept {
    return wait_until(task, &reached_state, state, deadline, poll);
}

namespace detail {

Status TaskAccess::claim(Task& task, const TaskState desired, TaskState& previous) noexcept {
    std::atomic<std::uint8_t>& word{task.control(key()).word};
    std::uint8_t expected{word.load(std::memory_order_acquire)};
    Status status{Status::kResourceLimit};
    bool decided{false};
    for (std::uint32_t attempt{0U}; (attempt < kMaxClaimAttempts) && (!decided); ++attempt) {
        const TaskState current{state_of(expected)};
        if (!is_claimable(current)) {
            status = Status::kAlreadyExists;
            decided = true;
        } else if (word.compare_exchange_weak(expected, bits_of(desired), std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            // The new word has no cancel bit: an old cancel request does not carry over.
            previous = current;
            status = Status::kOk;
            decided = true;
        } else {
            // expected was reloaded: try again.
        }
    }
    return status;
}

void TaskAccess::set_state(Task& task, const TaskState state) noexcept {
    std::atomic<std::uint8_t>& word{task.control(key()).word};
    std::uint8_t expected{word.load(std::memory_order_relaxed)};
    bool stored{false};
    for (std::uint32_t attempt{0U}; (attempt < kMaxSetStateAttempts) && (!stored); ++attempt) {
        const std::uint8_t desired{static_cast<std::uint8_t>((expected & kCancelBit) | bits_of(state))};
        stored = word.compare_exchange_strong(expected, desired, std::memory_order_release, std::memory_order_relaxed);
    }
    COMMON_ASSERT(stored);
}

bool TaskAccess::take_trigger(Task& task) noexcept {
    return task.control(key()).trigger.exchange(false, std::memory_order_acquire);
}

void TaskAccess::clear_trigger(Task& task) noexcept {
    task.control(key()).trigger.store(false, std::memory_order_relaxed);
}

bool TaskAccess::suspension_requested(Task& task) noexcept {
    return task.control(key()).suspend_requested.load(std::memory_order_acquire);
}

void TaskAccess::request_suspension(Task& task, const bool suspended) noexcept {
    task.control(key()).suspend_requested.store(suspended, std::memory_order_release);
}

void TaskAccess::set_submission(Task& task, const TimePoint release, const Deadline& deadline) noexcept {
    TaskControl& control{task.control(key())};
    control.release_ns = release.time_since_epoch().count();
    control.deadline = deadline;
}

TimePoint TaskAccess::submission_release(Task& task) noexcept {
    return TimePoint{Duration{task.control(key()).release_ns}};
}

Deadline TaskAccess::submission_deadline(Task& task) noexcept {
    return task.control(key()).deadline;
}

std::uint64_t TaskAccess::runs(Task& task) noexcept {
    return task.control(key()).statistics.runs;
}

Status TaskAccess::execute(Task& task, const TaskActivation& activation, const Duration budget,
                           const TaskClock clock) noexcept {
    TaskControl& control{task.control(key())};
    const Status status{task.run_activation(key(), activation)};
    const TimePoint finish{clock()};

    const std::int64_t start_ns{activation.now.time_since_epoch().count()};
    const std::int64_t finish_ns{finish.time_since_epoch().count()};
    const std::int64_t elapsed_ns{saturating_sub(finish_ns, start_ns)};
    const std::int64_t execution_ns{(elapsed_ns > 0) ? elapsed_ns : std::int64_t{0}};
    const std::int64_t late_ns{saturating_sub(start_ns, activation.release.time_since_epoch().count())};
    const std::int64_t lateness_ns{(late_ns > 0) ? late_ns : std::int64_t{0}};

    TaskStatistics& statistics{control.statistics};
    statistics.min_execution_ns = ((statistics.runs == 0U) || (execution_ns < statistics.min_execution_ns))
                                      ? execution_ns
                                      : statistics.min_execution_ns;
    statistics.runs = saturating_add(statistics.runs, std::uint64_t{1U});
    if (!is_ok(status)) {
        statistics.failures = saturating_add(statistics.failures, std::uint64_t{1U});
    }
    if ((budget > Duration::zero()) && (execution_ns > budget.count())) {
        statistics.overruns = saturating_add(statistics.overruns, std::uint64_t{1U});
    }
    if ((!activation.deadline.is_never()) && (finish > activation.deadline.time_point())) {
        statistics.deadline_misses = saturating_add(statistics.deadline_misses, std::uint64_t{1U});
    }
    statistics.missed_activations = saturating_add(statistics.missed_activations, activation.missed);
    statistics.last_execution_ns = execution_ns;
    statistics.max_execution_ns = (execution_ns > statistics.max_execution_ns) ? execution_ns : statistics.max_execution_ns;
    statistics.total_execution_ns = saturating_add(statistics.total_execution_ns, execution_ns);
    statistics.max_lateness_ns = (lateness_ns > statistics.max_lateness_ns) ? lateness_ns : statistics.max_lateness_ns;
    statistics.last_status = status;
    control.published.store(statistics);
    return status;
}

RunnerScope::RunnerScope(const void* const runner) noexcept : previous_{g_current_runner} {
    g_current_runner = runner;
}

RunnerScope::~RunnerScope() {
    g_current_runner = previous_;
}

const void* current_runner() noexcept {
    return g_current_runner;
}

}  // namespace detail

}  // namespace common
