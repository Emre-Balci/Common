#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "common/core/status.hpp"
#include "common/system/thread.hpp"
#include "common/task/task.hpp"
#include "common/task/task_scheduler.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

std::int64_t g_now_ns{0};  // The fake clock.

common::TimePoint fake_now() noexcept {
    return common::TimePoint{common::Duration{g_now_ns}};
}

common::TimePoint at(const common::Duration time) noexcept {
    g_now_ns = time.count();
    return fake_now();
}

/// The order in which tasks ran.
struct RunLog final {
    std::array<std::int32_t, 64U> ids{};
    std::size_t size{0U};

    void add(const std::int32_t id) noexcept {
        if (size < ids.size()) {
            ids[size] = id;
            ++size;
        }
    }
};

/// Logs its id when it runs; optionally takes fake time, or acts on another task.
class LogTask final : public common::Task {
public:
    LogTask(RunLog& log, const std::int32_t id) noexcept : Task{"log"}, log_{&log}, id_{id} {}

    common::Duration work{};
    common::Task* suspend_other{nullptr};
    bool suspend_self{false};
    bool trigger_self_once{false};
    common::TaskActivation last{common::TimePoint{}, common::TimePoint{}, common::Deadline::never(), 0U, 0U};
    std::int32_t runs{0};

private:
    common::Status run(const common::TaskActivation& activation) noexcept override {
        log_->add(id_);
        last = activation;
        ++runs;
        g_now_ns += work.count();
        if (suspend_other != nullptr) {
            suspend_other->suspend();
        }
        if (suspend_self) {
            suspend();
        }
        if (trigger_self_once) {
            trigger_self_once = false;
            trigger();
        }
        return common::Status::kOk;
    }

    RunLog* log_;
    std::int32_t id_;
};

/// Counts its runs from any thread (for the tests with real threads).
class CountingTask final : public common::Task {
public:
    CountingTask() noexcept : Task{"counting"} {}
    std::atomic<std::int32_t> runs{0};

private:
    common::Status run(const common::TaskActivation& /*activation*/) noexcept override {
        runs.fetch_add(1, std::memory_order_relaxed);
        return common::Status::kOk;
    }
};

common::TaskStatistics statistics_of(const common::Task& task) noexcept {
    common::TaskStatistics statistics{};
    static_cast<void>(task.try_statistics(statistics));
    return statistics;
}

common::ThreadConfig worker_config() noexcept {
    common::ThreadConfig config{};
    config.name = "test_tasks";
    config.policy = common::SchedPolicy::kOther;
    config.priority = 0;
    return config;
}

}  // namespace

TEST_CASE(task_scheduler_periods_offsets_and_priorities) {
    RunLog log{};
    LogTask a{log, 1};
    LogTask b{log, 2};
    LogTask c{log, 3};
    common::TaskScheduler<4U> scheduler{&fake_now};
    common::TaskSchedule schedule_a{common::TaskSchedule::periodic(10ms)};
    schedule_a.priority = 1U;
    common::TaskSchedule schedule_b{common::TaskSchedule::periodic(10ms, 5ms)};
    schedule_b.priority = 2U;
    common::TaskSchedule schedule_c{common::TaskSchedule::periodic(20ms)};
    schedule_c.priority = 2U;
    REQUIRE(scheduler.add(a, schedule_a) == common::Status::kOk);
    REQUIRE(scheduler.add(b, schedule_b) == common::Status::kOk);
    REQUIRE(scheduler.add(c, schedule_c) == common::Status::kOk);
    CHECK(scheduler.size() == 3U);
    CHECK(a.state() == common::TaskState::kScheduled);
    CHECK(scheduler.next_activation() == common::TimePoint::max());  // Not aligned yet.

    scheduler.align(at(0ms));
    CHECK(scheduler.next_activation() == common::TimePoint{0ms});
    CHECK(scheduler.run_once(at(0ms)) == 2U);  // c (priority 2) before a (priority 1); b is at 5 ms.
    CHECK(scheduler.run_once(at(5ms)) == 1U);  // b
    CHECK(scheduler.run_once(at(7ms)) == 0U);
    CHECK(scheduler.run_once(at(10ms)) == 1U);  // a; c is at 20 ms
    CHECK(scheduler.run_once(at(20ms)) == 3U);  // b's overdue 15 ms activation first (priority), then c, a
    REQUIRE(log.size == 7U);
    const std::array<std::int32_t, 7U> expected{{3, 1, 2, 1, 2, 3, 1}};
    for (std::size_t i{0U}; i < expected.size(); ++i) {
        CHECK(log.ids[i] == expected[i]);
    }
    CHECK(b.last.release == common::TimePoint{15ms});  // b ran at 20 ms for its 15 ms activation.
    CHECK(scheduler.next_activation() == common::TimePoint{25ms});
}

TEST_CASE(task_scheduler_missed_activations_after_a_stall) {
    RunLog log{};
    LogTask task{log, 1};
    common::TaskScheduler<2U> scheduler{&fake_now};
    REQUIRE(scheduler.add(task, common::TaskSchedule::periodic(10ms)) == common::Status::kOk);
    scheduler.align(at(0ms));
    CHECK(scheduler.run_once(at(0ms)) == 1U);
    // Stalled until 35 ms: the 30 ms point runs late, 10 and 20 are missed, no catch-up burst.
    CHECK(scheduler.run_once(at(35ms)) == 1U);
    CHECK(task.last.release == common::TimePoint{30ms});
    CHECK(task.last.missed == 2U);
    CHECK(task.last.deadline.time_point() == common::TimePoint{40ms});
    CHECK(scheduler.next_activation() == common::TimePoint{40ms});
    CHECK(scheduler.run_once(at(36ms)) == 0U);
    const common::TaskStatistics statistics{statistics_of(task)};
    CHECK(statistics.runs == 2U);
    CHECK(statistics.missed_activations == 2U);
    CHECK(statistics.max_lateness_ns == 5'000'000);
}

TEST_CASE(task_scheduler_overruns_and_deadline_misses) {
    RunLog log{};
    LogTask task{log, 1};
    task.work = 3ms;
    common::TaskSchedule schedule{common::TaskSchedule::periodic(10ms)};
    schedule.budget = 2ms;
    schedule.deadline = 2ms;
    common::TaskScheduler<1U> scheduler{&fake_now};
    REQUIRE(scheduler.add(task, schedule) == common::Status::kOk);
    scheduler.align(at(0ms));
    CHECK(scheduler.run_once(at(0ms)) == 1U);
    task.work = 1ms;
    CHECK(scheduler.run_once(at(10ms)) == 1U);
    const common::TaskStatistics statistics{statistics_of(task)};
    CHECK(statistics.overruns == 1U);
    CHECK(statistics.deadline_misses == 1U);
    CHECK(statistics.max_execution_ns == 3'000'000);
    CHECK(statistics.min_execution_ns == 1'000'000);
}

TEST_CASE(task_scheduler_triggered_tasks) {
    RunLog log{};
    LogTask task{log, 1};
    common::TaskSchedule schedule{common::TaskSchedule::triggered()};
    schedule.deadline = 4ms;
    common::TaskScheduler<1U> scheduler{&fake_now};
    REQUIRE(scheduler.add(task, schedule) == common::Status::kOk);
    scheduler.align(at(0ms));
    CHECK(scheduler.run_once(at(1ms)) == 0U);
    CHECK(scheduler.next_activation() == common::TimePoint::max());

    task.trigger();
    task.trigger();  // Several triggers before it runs count once.
    CHECK(scheduler.run_once(at(2ms)) == 1U);
    CHECK(task.last.release == common::TimePoint{2ms});
    CHECK(task.last.deadline.time_point() == common::TimePoint{6ms});
    CHECK(task.last.missed == 0U);
    CHECK(scheduler.run_once(at(3ms)) == 0U);

    // A trigger made during its own run gives one more run at the next pass.
    task.trigger_self_once = true;
    task.trigger();
    CHECK(scheduler.run_once(at(4ms)) == 1U);
    CHECK(scheduler.run_once(at(5ms)) == 1U);
    CHECK(scheduler.run_once(at(6ms)) == 0U);
    CHECK(task.runs == 3);
}

TEST_CASE(task_scheduler_trigger_from_another_thread) {
    static CountingTask task{};
    common::TaskScheduler<1U> scheduler{&fake_now};
    REQUIRE(scheduler.add(task, common::TaskSchedule::triggered()) == common::Status::kOk);
    scheduler.align(at(0ms));
    std::atomic<bool> done{false};
    std::thread trigger_thread{[&done]() {
        for (std::int32_t i{0}; i < 2000; ++i) {
            task.trigger();
        }
        done.store(true, std::memory_order_release);
    }};
    std::int32_t passes{0};
    for (std::int32_t i{0}; (i < 1'000'000) && (!done.load(std::memory_order_acquire)); ++i) {
        static_cast<void>(scheduler.run_once(at(1ms)));
        ++passes;
    }
    trigger_thread.join();
    static_cast<void>(scheduler.run_once(at(2ms)));  // The last trigger, if it came after the last pass.
    const std::int32_t runs{task.runs.load(std::memory_order_relaxed)};
    CHECK(runs >= 1);
    CHECK(runs <= 2000);
    CHECK(scheduler.run_once(at(3ms)) == 0U);  // Every trigger was consumed.
    CHECK(scheduler.remove(task) == common::Status::kOk);
    static_cast<void>(passes);
}

TEST_CASE(task_scheduler_suspend_and_resume) {
    RunLog log{};
    LogTask task{log, 1};
    common::TaskScheduler<1U> scheduler{&fake_now};
    REQUIRE(scheduler.add(task, common::TaskSchedule::periodic(10ms)) == common::Status::kOk);
    scheduler.align(at(0ms));
    CHECK(scheduler.run_once(at(0ms)) == 1U);

    task.suspend();
    CHECK(task.state() == common::TaskState::kScheduled);  // A request: applied at the next pass.
    CHECK(scheduler.run_once(at(10ms)) == 0U);
    CHECK(task.state() == common::TaskState::kSuspended);
    task.trigger();  // Dropped while suspended.
    CHECK(scheduler.run_once(at(20ms)) == 0U);
    CHECK(scheduler.next_activation() == common::TimePoint::max());

    task.resume();
    CHECK(scheduler.run_once(at(43ms)) == 0U);  // Back on the grid at 50 ms; nothing missed, no trigger.
    CHECK(task.state() == common::TaskState::kScheduled);
    CHECK(scheduler.next_activation() == common::TimePoint{50ms});
    CHECK(scheduler.run_once(at(50ms)) == 1U);
    CHECK(task.last.missed == 0U);
    CHECK(statistics_of(task).missed_activations == 0U);
}

TEST_CASE(task_scheduler_suspensions_within_a_pass) {
    RunLog log{};
    LogTask high{log, 1};
    LogTask low{log, 2};
    LogTask self{log, 3};
    high.suspend_other = &low;
    self.suspend_self = true;
    common::TaskSchedule high_schedule{common::TaskSchedule::periodic(10ms)};
    high_schedule.priority = 9U;
    common::TaskScheduler<3U> scheduler{&fake_now};
    REQUIRE(scheduler.add(high, high_schedule) == common::Status::kOk);
    REQUIRE(scheduler.add(low, common::TaskSchedule::periodic(10ms)) == common::Status::kOk);
    REQUIRE(scheduler.add(self, common::TaskSchedule::periodic(10ms)) == common::Status::kOk);
    scheduler.align(at(0ms));
    CHECK(scheduler.run_once(at(0ms)) == 2U);  // high, then self; low was suspended by high in this pass.
    CHECK(low.state() == common::TaskState::kSuspended);
    CHECK(self.state() == common::TaskState::kSuspended);  // Suspended itself during its run.
    CHECK(low.runs == 0);
    REQUIRE(log.size == 2U);
    CHECK(log.ids[0U] == 1);
    CHECK(log.ids[1U] == 3);
}

TEST_CASE(task_scheduler_add_remove_and_errors) {
    RunLog log{};
    LogTask a{log, 1};
    LogTask b{log, 2};
    LogTask c{log, 3};
    common::TaskScheduler<2U> scheduler{&fake_now};
    CHECK(scheduler.capacity() == 2U);

    common::TaskSchedule negative{common::TaskSchedule::periodic(10ms)};
    negative.offset = -1ms;
    CHECK(scheduler.add(a, negative) == common::Status::kInvalidArgument);
    CHECK(scheduler.add(a, common::TaskSchedule{}) == common::Status::kInvalidArgument);  // No period.
    common::TaskSchedule both{common::TaskSchedule::periodic(10ms)};
    both.triggered_only = true;
    CHECK(scheduler.add(a, both) == common::Status::kInvalidArgument);
    CHECK(a.state() == common::TaskState::kIdle);

    REQUIRE(scheduler.add(a, common::TaskSchedule::periodic(10ms)) == common::Status::kOk);
    CHECK(scheduler.add(a, common::TaskSchedule::periodic(10ms)) == common::Status::kAlreadyExists);
    common::TaskSchedule suspended{common::TaskSchedule::periodic(10ms)};
    suspended.suspended = true;
    REQUIRE(scheduler.add(b, suspended) == common::Status::kOk);
    CHECK(b.state() == common::TaskState::kSuspended);
    CHECK(scheduler.add(c, common::TaskSchedule::periodic(10ms)) == common::Status::kResourceLimit);
    CHECK(c.state() == common::TaskState::kIdle);

    CHECK(scheduler.remove(c) == common::Status::kNotFound);
    CHECK(scheduler.remove(a) == common::Status::kOk);
    CHECK(a.state() == common::TaskState::kIdle);
    CHECK(scheduler.size() == 1U);
}

TEST_CASE(task_scheduler_add_after_align) {
    RunLog log{};
    LogTask early{log, 1};
    LogTask late{log, 2};
    common::TaskScheduler<2U> scheduler{&fake_now};
    REQUIRE(scheduler.add(early, common::TaskSchedule::periodic(10ms)) == common::Status::kOk);
    scheduler.align(at(0ms));
    CHECK(scheduler.run_once(at(23ms)) == 1U);
    // Added at the 23 ms pass: it joins the grid at 30 ms; the activations before it are not missed.
    REQUIRE(scheduler.add(late, common::TaskSchedule::periodic(10ms)) == common::Status::kOk);
    CHECK(scheduler.run_once(at(30ms)) == 2U);
    CHECK(late.last.missed == 0U);
    CHECK(late.last.release == common::TimePoint{30ms});
}

TEST_CASE(task_scheduler_own_thread) {
    static CountingTask task{};
    static common::TaskScheduler<1U> scheduler{};
    REQUIRE(scheduler.add(task, common::TaskSchedule::periodic(2ms)) == common::Status::kOk);
    CHECK(scheduler.heartbeat().last_beat() == common::TimePoint::max());  // Parked before it runs.
    CHECK(scheduler.start(worker_config(), 0ms) == common::Status::kInvalidArgument);
    REQUIRE(scheduler.start(worker_config()) == common::Status::kOk);
    CHECK(scheduler.running());
    CHECK(scheduler.start(worker_config()) == common::Status::kAlreadyRunning);

    const common::Deadline deadline{common::Deadline::after(10s)};
    for (std::int32_t i{0}; (i < 10'000) && (task.runs.load(std::memory_order_relaxed) < 3) && (!deadline.expired());
         ++i) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(task.runs.load(std::memory_order_relaxed) >= 3);

    // A suspension requested from this thread is applied by the scheduler's thread.
    task.suspend();
    CHECK(common::wait_until_state(task, common::TaskState::kSuspended, common::Deadline::after(10s)) ==
          common::Status::kOk);
    task.resume();
    CHECK(common::wait_until_state(task, common::TaskState::kScheduled, common::Deadline::after(10s)) ==
          common::Status::kOk);

    CHECK(scheduler.stop() == common::Status::kOk);
    CHECK(!scheduler.running());
    CHECK(scheduler.stop() == common::Status::kNotRunning);
    CHECK(scheduler.heartbeat().last_beat() == common::TimePoint::max());  // Parked again.

    // Started again after stop().
    const std::int32_t before{task.runs.load(std::memory_order_relaxed)};
    REQUIRE(scheduler.start(worker_config()) == common::Status::kOk);
    const common::Deadline again{common::Deadline::after(10s)};
    for (std::int32_t i{0}; (i < 10'000) && (task.runs.load(std::memory_order_relaxed) <= before) && (!again.expired());
         ++i) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(task.runs.load(std::memory_order_relaxed) > before);
    CHECK(scheduler.stop() == common::Status::kOk);
    CHECK(scheduler.remove(task) == common::Status::kOk);
}
