#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "common/core/status.hpp"
#include "common/system/thread.hpp"
#include "common/task/task.hpp"
#include "common/task/task_executor.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

using Wait = bool (*)() noexcept;

/// A job: counts its runs, returns `result`, and can wait for a condition (bounded) or for a cancel.
class Job final : public common::Task {
public:
    Job() noexcept : Task{"job"} {}

    std::atomic<std::int32_t> runs{0};
    std::atomic<bool> started{false};
    common::Status result{common::Status::kOk};
    Wait wait_until{nullptr};      // Returns from run() once it is true (at most about 10 s).
    bool wait_for_cancel{false};   // Returns kInterrupted once cancel_requested() (at most about 10 s).

private:
    common::Status run(const common::TaskActivation& /*activation*/) noexcept override {
        started.store(true, std::memory_order_release);
        runs.fetch_add(1, std::memory_order_relaxed);
        common::Status status{result};
        if (wait_for_cancel) {
            for (std::int32_t i{0}; (i < 100'000) && (!cancel_requested()); ++i) {
                std::this_thread::sleep_for(100us);
            }
            status = cancel_requested() ? common::Status::kInterrupted : common::Status::kOk;
        } else if (wait_until != nullptr) {
            for (std::int32_t i{0}; (i < 100'000) && (!wait_until()); ++i) {
                std::this_thread::sleep_for(100us);
            }
        } else {
            // Returns at once.
        }
        return status;
    }
};

common::ThreadConfig worker_config() noexcept {
    common::ThreadConfig config{};
    config.name = "test_workers";
    config.policy = common::SchedPolicy::kOther;
    config.priority = 0;
    return config;
}

bool wait_started(const Job& job) noexcept {
    for (std::int32_t i{0}; (i < 100'000) && (!job.started.load(std::memory_order_acquire)); ++i) {
        std::this_thread::sleep_for(100us);
    }
    return job.started.load(std::memory_order_acquire);
}

common::Status finish(const common::Task& task) noexcept {
    return common::wait_until_finished(task, common::Deadline::after(10s));
}

// For the kCancel test: the blocking job returns once the executor is stopping.
common::TaskExecutor<1U, 8U> g_cancel_executor{};

bool cancel_executor_stopping() noexcept {
    return g_cancel_executor.stopping();
}

}  // namespace

TEST_CASE(task_executor_runs_jobs_on_workers) {
    static std::array<Job, 10U> jobs{};
    static common::TaskExecutor<2U, 16U> executor{};
    CHECK(executor.heartbeat(0U).last_beat() == common::TimePoint::max());
    REQUIRE(executor.start(worker_config()) == common::Status::kOk);
    CHECK(executor.running());
    CHECK(executor.start(worker_config()) == common::Status::kAlreadyRunning);
    jobs[3U].result = common::Status::kTimeout;
    for (Job& job : jobs) {
        CHECK(executor.submit(job) == common::Status::kOk);
    }
    for (Job& job : jobs) {
        CHECK(finish(job) == common::Status::kOk);
        CHECK(job.runs.load(std::memory_order_relaxed) == 1);
    }
    CHECK(jobs[0U].state() == common::TaskState::kDone);
    CHECK(jobs[3U].state() == common::TaskState::kFailed);
    common::TaskStatistics statistics{};
    REQUIRE(jobs[3U].try_statistics(statistics));
    CHECK(statistics.runs == 1U);
    CHECK(statistics.failures == 1U);
    CHECK(statistics.last_status == common::Status::kTimeout);
    CHECK(executor.heartbeat(1U).last_beat() != common::TimePoint::max());  // The workers beat.
    CHECK(executor.stop() == common::Status::kOk);
    CHECK(!executor.running());
    CHECK(executor.heartbeat(0U).last_beat() == common::TimePoint::max());  // Parked again.
}

TEST_CASE(task_executor_run_pending_resubmit_and_errors) {
    Job job{};
    Job other{};
    common::TaskExecutor<1U, 2U> executor{};
    CHECK(executor.submit(job) == common::Status::kOk);
    CHECK(job.state() == common::TaskState::kQueued);
    CHECK(executor.submit(job) == common::Status::kAlreadyExists);  // Already queued.
    CHECK(executor.submit(other) == common::Status::kOk);
    Job third{};
    CHECK(executor.submit(third) == common::Status::kResourceLimit);  // The queue holds 2.
    CHECK(third.state() == common::TaskState::kIdle);                 // Left as it was.

    CHECK(executor.run_pending(1U) == 1U);
    CHECK(job.state() == common::TaskState::kDone);
    CHECK(executor.run_pending(8U) == 1U);
    CHECK(other.state() == common::TaskState::kDone);
    CHECK(executor.run_pending(8U) == 0U);

    // A finished job can be submitted again.
    CHECK(executor.submit(job) == common::Status::kOk);
    CHECK(executor.run_pending(1U) == 1U);
    CHECK(job.runs.load(std::memory_order_relaxed) == 2);
}

TEST_CASE(task_executor_cancel_before_and_while_running) {
    // Before it runs: it stays queued until it is dequeued, then becomes cancelled without running.
    Job queued{};
    common::TaskExecutor<1U, 4U> idle{};
    REQUIRE(idle.submit(queued) == common::Status::kOk);
    queued.cancel();
    CHECK(queued.state() == common::TaskState::kQueued);
    CHECK(idle.run_pending(1U) == 1U);
    CHECK(queued.state() == common::TaskState::kCancelled);
    CHECK(queued.runs.load(std::memory_order_relaxed) == 0);

    // A new submission clears the old cancel request.
    REQUIRE(idle.submit(queued) == common::Status::kOk);
    CHECK(!queued.cancel_requested());
    CHECK(idle.run_pending(1U) == 1U);
    CHECK(queued.state() == common::TaskState::kDone);

    // While it runs: the job sees the request, returns an error, and ends cancelled.
    static Job running{};
    running.wait_for_cancel = true;
    static common::TaskExecutor<1U, 4U> executor{};
    REQUIRE(executor.start(worker_config()) == common::Status::kOk);
    REQUIRE(executor.submit(running) == common::Status::kOk);
    REQUIRE(wait_started(running));
    running.cancel();
    CHECK(finish(running) == common::Status::kOk);
    CHECK(running.state() == common::TaskState::kCancelled);
    CHECK(executor.stop() == common::Status::kOk);
}

TEST_CASE(task_executor_stop_drains_accepted_jobs) {
    static std::array<Job, 6U> jobs{};
    static common::TaskExecutor<2U, 8U> executor{};
    REQUIRE(executor.start(worker_config()) == common::Status::kOk);
    for (Job& job : jobs) {
        REQUIRE(executor.submit(job) == common::Status::kOk);
    }
    CHECK(executor.stop(common::StopMode::kDrain) == common::Status::kOk);
    for (const Job& job : jobs) {
        CHECK(job.state() == common::TaskState::kDone);  // Every accepted job ran before stop() returned.
    }
    Job late{};
    CHECK(executor.submit(late) == common::Status::kNotRunning);
    CHECK(executor.stopping());
    CHECK(executor.stop() == common::Status::kNotRunning);

    // Started again after stop().
    REQUIRE(executor.start(worker_config()) == common::Status::kOk);
    CHECK(!executor.stopping());
    REQUIRE(executor.submit(late) == common::Status::kOk);
    CHECK(finish(late) == common::Status::kOk);
    CHECK(executor.stop() == common::Status::kOk);
}

TEST_CASE(task_executor_stop_cancel_cancels_queued_jobs) {
    static Job blocking{};
    static std::array<Job, 3U> waiting{};
    blocking.wait_until = &cancel_executor_stopping;
    REQUIRE(g_cancel_executor.start(worker_config()) == common::Status::kOk);
    REQUIRE(g_cancel_executor.submit(blocking) == common::Status::kOk);
    REQUIRE(wait_started(blocking));
    for (Job& job : waiting) {
        REQUIRE(g_cancel_executor.submit(job) == common::Status::kOk);
    }
    // The single worker is busy with the blocking job, which returns once stop() has begun; the
    // worker then exits without taking another job.
    CHECK(g_cancel_executor.stop(common::StopMode::kCancel) == common::Status::kOk);
    CHECK(blocking.state() == common::TaskState::kDone);
    for (const Job& job : waiting) {
        CHECK(job.state() == common::TaskState::kCancelled);
        CHECK(job.runs.load(std::memory_order_relaxed) == 0);
    }
}

TEST_CASE(task_executor_stop_without_workers_cancels) {
    Job job{};
    common::TaskExecutor<1U, 4U> executor{};
    REQUIRE(executor.submit(job) == common::Status::kOk);
    CHECK(executor.stop(common::StopMode::kDrain) == common::Status::kNotRunning);
    CHECK(job.state() == common::TaskState::kCancelled);  // Never run on the control thread.
}

TEST_CASE(task_executor_submits_racing_stop) {
    // Several producers keep submitting their jobs while stop() runs: afterwards every job is finished
    // or idle, none is left queued or running, and later submits are refused.
    constexpr std::size_t kProducers{4U};
    constexpr std::size_t kJobsPerProducer{8U};
    static std::array<std::array<Job, kJobsPerProducer>, kProducers> jobs{};
    static common::TaskExecutor<2U, 16U> executor{};
    REQUIRE(executor.start(worker_config()) == common::Status::kOk);
    std::atomic<std::int32_t> refused{0};
    std::array<std::thread, kProducers> producers{};
    for (std::size_t p{0U}; p < kProducers; ++p) {
        producers[p] = std::thread{[p, &refused]() {
            bool stopped{false};
            const common::Deadline deadline{common::Deadline::after(10s)};
            for (std::int32_t round{0}; (round < 100'000'000) && (!stopped) && (!deadline.expired()); ++round) {
                std::this_thread::yield();
                for (Job& job : jobs[p]) {
                    const common::TaskState state{job.state()};
                    if ((state == common::TaskState::kIdle) || common::is_finished(state)) {
                        if (executor.submit(job) == common::Status::kNotRunning) {
                            stopped = true;
                        }
                    }
                }
            }
            if (stopped) {
                refused.fetch_add(1, std::memory_order_relaxed);
            }
        }};
    }
    std::this_thread::sleep_for(20ms);
    CHECK(executor.stop(common::StopMode::kDrain) == common::Status::kOk);
    for (std::thread& producer : producers) {
        producer.join();
    }
    CHECK(refused.load(std::memory_order_relaxed) == static_cast<std::int32_t>(kProducers));
    std::int32_t total_runs{0};
    for (const std::array<Job, kJobsPerProducer>& group : jobs) {
        for (const Job& job : group) {
            const common::TaskState state{job.state()};
            CHECK((state == common::TaskState::kIdle) || common::is_finished(state));
            total_runs += job.runs.load(std::memory_order_relaxed);
        }
    }
    CHECK(total_runs > 0);
}
