#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>

#include "common/core/status.hpp"
#include "common/task/task.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

// A fake clock: tests set the time, and tasks advance it while they "run".
std::int64_t g_fake_ns{0};

common::TimePoint fake_now() noexcept {
    return common::TimePoint{common::Duration{g_fake_ns}};
}

constexpr common::TimePoint at(const std::int64_t ns) noexcept {
    return common::TimePoint{common::Duration{ns}};
}

/// Takes `work_ns` of fake time per run and returns `result`.
class StepTask final : public common::Task {
public:
    StepTask() noexcept : Task{"step"} {}

    std::int64_t work_ns{0};
    common::Status result{common::Status::kOk};
    std::uint64_t last_sequence{0U};
    std::uint64_t last_missed{0U};

private:
    common::Status run(const common::TaskActivation& activation) noexcept override {
        last_sequence = activation.sequence;
        last_missed = activation.missed;
        g_fake_ns += work_ns;
        return result;
    }
};

common::TaskActivation activation_at(const std::int64_t now_ns, const std::int64_t release_ns,
                                     const common::Deadline deadline = common::Deadline::never(),
                                     const std::uint64_t missed = 0U) noexcept {
    return common::TaskActivation{at(now_ns), at(release_ns), deadline, 0U, missed};
}

common::TaskStatistics statistics_of(const common::Task& task) noexcept {
    common::TaskStatistics statistics{};
    static_cast<void>(task.try_statistics(statistics));
    return statistics;
}

}  // namespace

TEST_CASE(task_initial_state_names_and_strings) {
    const StepTask task{};
    CHECK(task.state() == common::TaskState::kIdle);
    CHECK(std::string_view{task.name()} == "step");
    CHECK(!task.cancel_requested());
    const common::TaskStatistics statistics{statistics_of(task)};
    CHECK(statistics.runs == 0U);
    CHECK(statistics.last_status == common::Status::kOk);

    CHECK(std::string_view{common::to_string(common::TaskState::kQueued)} == "queued");
    CHECK(std::string_view{common::to_string(common::TaskState::kCancelled)} == "cancelled");
    CHECK(common::is_finished(common::TaskState::kDone));
    CHECK(common::is_finished(common::TaskState::kFailed));
    CHECK(common::is_finished(common::TaskState::kCancelled));
    CHECK(!common::is_finished(common::TaskState::kRunning));
    CHECK(!common::is_finished(common::TaskState::kIdle));
}

TEST_CASE(task_statistics_arithmetic) {
    StepTask task{};
    using common::detail::TaskAccess;

    // Run 1: starts 2 ms late, works 3 ms, budget 2 ms (overrun), deadline at 4 ms (missed).
    g_fake_ns = 2'000'000;
    task.work_ns = 3'000'000;
    CHECK(TaskAccess::execute(task, activation_at(2'000'000, 0, common::Deadline::at(at(4'000'000)), 1U), 2ms,
                              &fake_now) == common::Status::kOk);
    common::TaskStatistics statistics{statistics_of(task)};
    CHECK(statistics.runs == 1U);
    CHECK(statistics.overruns == 1U);
    CHECK(statistics.deadline_misses == 1U);
    CHECK(statistics.missed_activations == 1U);
    CHECK(statistics.last_execution_ns == 3'000'000);
    CHECK(statistics.min_execution_ns == 3'000'000);
    CHECK(statistics.max_execution_ns == 3'000'000);
    CHECK(statistics.max_lateness_ns == 2'000'000);

    // Run 2: on time, 1 ms, fails, within budget and deadline.
    g_fake_ns = 10'000'000;
    task.work_ns = 1'000'000;
    task.result = common::Status::kTimeout;
    CHECK(TaskAccess::execute(task, activation_at(10'000'000, 10'000'000, common::Deadline::at(at(20'000'000))), 2ms,
                              &fake_now) == common::Status::kTimeout);
    statistics = statistics_of(task);
    CHECK(statistics.runs == 2U);
    CHECK(statistics.failures == 1U);
    CHECK(statistics.overruns == 1U);
    CHECK(statistics.deadline_misses == 1U);
    CHECK(statistics.min_execution_ns == 1'000'000);
    CHECK(statistics.max_execution_ns == 3'000'000);
    CHECK(statistics.total_execution_ns == 4'000'000);
    CHECK(statistics.last_status == common::Status::kTimeout);

    // Run 3: a release after the start (another clock's jitter) counts as no lateness; no budget.
    g_fake_ns = 20'000'000;
    task.work_ns = 5'000'000;
    task.result = common::Status::kOk;
    CHECK(TaskAccess::execute(task, activation_at(20'000'000, 21'000'000), common::Duration::zero(), &fake_now) ==
          common::Status::kOk);
    statistics = statistics_of(task);
    CHECK(statistics.overruns == 1U);  // Zero budget: no overrun.
    CHECK(statistics.max_lateness_ns == 2'000'000);
    CHECK(statistics.last_status == common::Status::kOk);
    CHECK(statistics.total_execution_ns == 9'000'000);
    CHECK(TaskAccess::runs(task) == 3U);
}

TEST_CASE(task_statistics_saturate) {
    StepTask task{};
    using common::detail::TaskAccess;
    constexpr std::int64_t kHuge{std::numeric_limits<std::int64_t>::max() / 2};
    for (std::int32_t i{0}; i < 3; ++i) {
        g_fake_ns = 0;
        task.work_ns = kHuge + 1;
        static_cast<void>(TaskAccess::execute(task, activation_at(0, 0), common::Duration::zero(), &fake_now));
    }
    const common::TaskStatistics statistics{statistics_of(task)};
    CHECK(statistics.total_execution_ns == std::numeric_limits<std::int64_t>::max());
    CHECK(statistics.runs == 3U);
}

TEST_CASE(task_claim_and_cancel_requests) {
    StepTask task{};
    using common::detail::TaskAccess;
    common::TaskState previous{common::TaskState::kRunning};

    CHECK(TaskAccess::claim(task, common::TaskState::kQueued, previous) == common::Status::kOk);
    CHECK(previous == common::TaskState::kIdle);
    CHECK(task.state() == common::TaskState::kQueued);
    CHECK(TaskAccess::claim(task, common::TaskState::kScheduled, previous) == common::Status::kAlreadyExists);

    // A cancel after the claim survives the state changes of the runner.
    task.cancel();
    CHECK(task.cancel_requested());
    TaskAccess::set_state(task, common::TaskState::kRunning);
    CHECK(task.state() == common::TaskState::kRunning);
    CHECK(task.cancel_requested());
    TaskAccess::set_state(task, common::TaskState::kCancelled);
    CHECK(task.state() == common::TaskState::kCancelled);

    // A finished task can be claimed again; the claim clears the old cancel request.
    CHECK(TaskAccess::claim(task, common::TaskState::kQueued, previous) == common::Status::kOk);
    CHECK(previous == common::TaskState::kCancelled);
    CHECK(!task.cancel_requested());
    TaskAccess::set_state(task, common::TaskState::kIdle);
}

TEST_CASE(task_trigger_and_suspension_requests) {
    StepTask task{};
    using common::detail::TaskAccess;
    CHECK(!TaskAccess::take_trigger(task));
    task.trigger();
    task.trigger();  // Coalesced.
    CHECK(TaskAccess::take_trigger(task));
    CHECK(!TaskAccess::take_trigger(task));
    task.trigger();
    TaskAccess::clear_trigger(task);
    CHECK(!TaskAccess::take_trigger(task));

    CHECK(!TaskAccess::suspension_requested(task));
    task.suspend();
    CHECK(TaskAccess::suspension_requested(task));
    task.resume();
    CHECK(!TaskAccess::suspension_requested(task));
    TaskAccess::request_suspension(task, true);
    CHECK(TaskAccess::suspension_requested(task));
    CHECK(task.state() == common::TaskState::kIdle);  // Requests never change the state.

    TaskAccess::set_submission(task, at(1234), common::Deadline::at(at(5678)));
    CHECK(TaskAccess::submission_release(task) == at(1234));
    CHECK(TaskAccess::submission_deadline(task).time_point() == at(5678));
}

TEST_CASE(task_wait_until) {
    StepTask task{};
    using common::detail::TaskAccess;
    CHECK(common::wait_until_finished(task, common::Deadline::after(1s), common::Duration::zero()) ==
          common::Status::kInvalidArgument);
    CHECK(common::wait_until_finished(task, common::Deadline::after(5ms)) == common::Status::kTimeout);
    CHECK(common::wait_until_state(task, common::TaskState::kIdle, common::Deadline{}) == common::Status::kOk);
    CHECK(common::wait_until_finished(task, common::Deadline{}) == common::Status::kTimeout);  // Expired at once.

    // Another thread finishes the task while this one waits.
    common::TaskState previous{};
    REQUIRE(TaskAccess::claim(task, common::TaskState::kQueued, previous) == common::Status::kOk);
    std::thread finisher{[&task]() {
        std::this_thread::sleep_for(20ms);
        TaskAccess::set_state(task, common::TaskState::kDone);
    }};
    CHECK(common::wait_until_finished(task, common::Deadline::after(10s)) == common::Status::kOk);
    CHECK(common::wait_until_state(task, common::TaskState::kDone, common::Deadline::never()) == common::Status::kOk);
    finisher.join();
}

TEST_CASE(task_runner_scopes_nest) {
    const std::int32_t first{1};
    const std::int32_t second{2};
    CHECK(common::detail::current_runner() == nullptr);
    {
        const common::detail::RunnerScope outer{&first};
        CHECK(common::detail::current_runner() == &first);
        {
            const common::detail::RunnerScope inner{&second};
            CHECK(common::detail::current_runner() == &second);
        }
        CHECK(common::detail::current_runner() == &first);
        std::thread other{[]() { CHECK(common::detail::current_runner() == nullptr); }};  // Per thread.
        other.join();
    }
    CHECK(common::detail::current_runner() == nullptr);
}

TEST_CASE(task_statistics_read_from_another_thread) {
    static StepTask task{};
    using common::detail::TaskAccess;
    std::atomic<bool> writing{true};
    std::atomic<bool> consistent{true};
    std::thread reader{[&]() {
        std::uint64_t last_runs{0U};
        for (std::uint32_t i{0U}; (i < 1'000'000U) && writing.load(std::memory_order_acquire); ++i) {
            common::TaskStatistics statistics{};
            if (task.try_statistics(statistics)) {
                // Every run takes 1 us: the total always matches the run count of the same snapshot.
                if ((statistics.runs < last_runs) ||
                    (statistics.total_execution_ns != static_cast<std::int64_t>(statistics.runs) * 1000)) {
                    consistent.store(false, std::memory_order_relaxed);
                }
                last_runs = statistics.runs;
            }
        }
    }};
    for (std::int32_t i{0}; i < 20'000; ++i) {
        const common::TaskActivation activation{at(0), at(0), common::Deadline::never(), 0U, 0U};
        static_cast<void>(TaskAccess::execute(
            task, activation, common::Duration::zero(),
            []() noexcept { return common::TimePoint{common::Duration{1000}}; }));  // Every run ends at 1 us.
    }
    writing.store(false, std::memory_order_release);
    reader.join();
    CHECK(consistent.load(std::memory_order_relaxed));
    CHECK(statistics_of(task).runs == 20'000U);
}
