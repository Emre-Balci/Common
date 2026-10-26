#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "common/core/status.hpp"
#include "common/functional/inplace_function.hpp"
#include "common/system/thread.hpp"
#include "common/task/active_object.hpp"
#include "common/task/task.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "common/time/timer_wheel.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

common::ThreadConfig object_config() noexcept {
    common::ThreadConfig config{};
    config.name = "test_active";
    config.policy = common::SchedPolicy::kOther;
    config.priority = 0;
    return config;
}

/// Waits (bounded) until @p done returns true.
template <typename Done>
bool eventually(const Done& done) noexcept {
    const common::Deadline deadline{common::Deadline::after(10s)};
    for (std::int32_t i{0}; (i < 1'000'000) && (!done()) && (!deadline.expired()); ++i) {
        std::this_thread::sleep_for(100us);
    }
    return done();
}

/// State that only the object's commands touch: no locks needed.
struct Sequence final {
    std::array<std::int32_t, 128U> values{};
    std::size_t size{0U};
    std::thread::id thread{};

    void add(const std::int32_t value) noexcept {
        if (size < values.size()) {
            values[size] = value;
            ++size;
        }
        thread = std::this_thread::get_id();
    }
};

}  // namespace

TEST_CASE(active_object_runs_commands_in_order_on_its_thread) {
    static common::ActiveObject<128U> active{};
    static Sequence sequence{};
    CHECK(active.heartbeat().last_beat() == common::TimePoint::max());
    REQUIRE(active.start(object_config()) == common::Status::kOk);
    CHECK(active.running());
    CHECK(active.start(object_config()) == common::Status::kAlreadyRunning);
    for (std::int32_t i{0}; i < 100; ++i) {
        REQUIRE(active.post([i]() noexcept { sequence.add(i); }) == common::Status::kOk);
    }
    CHECK(eventually([]() noexcept { return active.executed() == 100U; }));
    CHECK(active.stop() == common::Status::kOk);
    REQUIRE(sequence.size == 100U);
    for (std::int32_t i{0}; i < 100; ++i) {
        CHECK(sequence.values[static_cast<std::size_t>(i)] == i);
    }
    CHECK(sequence.thread != std::this_thread::get_id());
    CHECK(active.heartbeat().last_beat() == common::TimePoint::max());  // Parked again.
}

TEST_CASE(active_object_posts_from_many_threads) {
    static common::ActiveObject<64U> active{};
    static std::int64_t total{0};  // Touched by the object's commands only.
    REQUIRE(active.start(object_config()) == common::Status::kOk);
    constexpr std::size_t kThreads{4U};
    constexpr std::int32_t kPosts{1000};
    std::array<std::thread, kThreads> producers{};
    for (std::thread& producer : producers) {
        producer = std::thread{[]() {
            for (std::int32_t i{0}; i < kPosts; ++i) {
                common::Status status{common::Status::kResourceLimit};
                for (std::int32_t attempt{0}; (attempt < 1'000'000) && (status == common::Status::kResourceLimit);
                     ++attempt) {
                    status = active.post([]() noexcept { ++total; });
                    if (status == common::Status::kResourceLimit) {
                        std::this_thread::yield();
                    }
                }
            }
        }};
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    CHECK(active.stop(common::StopMode::kDrain) == common::Status::kOk);
    CHECK(total == static_cast<std::int64_t>(kThreads) * kPosts);
    CHECK(active.executed() == static_cast<std::uint64_t>(kThreads) * kPosts);
}

TEST_CASE(active_object_without_a_thread) {
    common::ActiveObject<4U> active{};
    std::int32_t value{0};
    CHECK(active.post([&value]() noexcept { value += 1; }) == common::Status::kOk);
    CHECK(active.post([&value, &active]() noexcept {
        value += 10;
        // A command may post: the new command runs after the ones already queued.
        CHECK(active.post([&value]() noexcept { value += 100; }) == common::Status::kOk);
    }) == common::Status::kOk);
    CHECK(active.post([&value]() noexcept { value *= 2; }) == common::Status::kOk);
    CHECK(active.post([]() noexcept {}) == common::Status::kOk);
    CHECK(active.post([]() noexcept {}) == common::Status::kResourceLimit);  // 4 queued.
    using Function = void (*)() noexcept;
    const Function none{nullptr};
    CHECK(active.post(none) == common::Status::kInvalidArgument);

    CHECK(active.run_pending(3U) == 3U);
    CHECK(value == 22);  // (1 + 10) * 2
    CHECK(active.run_pending(10U) == 2U);
    CHECK(value == 122);
    CHECK(active.executed() == 5U);
}

TEST_CASE(active_object_stop_drains_and_refuses) {
    static common::ActiveObject<16U> active{};
    static std::atomic<std::int32_t> ran{0};
    REQUIRE(active.start(object_config()) == common::Status::kOk);
    for (std::int32_t i{0}; i < 10; ++i) {
        REQUIRE(active.post([]() noexcept { ran.fetch_add(1, std::memory_order_relaxed); }) == common::Status::kOk);
    }
    CHECK(active.stop(common::StopMode::kDrain) == common::Status::kOk);
    CHECK(ran.load(std::memory_order_relaxed) == 10);  // Everything accepted ran before stop() returned.
    CHECK(active.stopping());
    CHECK(active.post([]() noexcept {}) == common::Status::kNotRunning);
    CHECK(active.stop() == common::Status::kNotRunning);

    // Started again after stop().
    REQUIRE(active.start(object_config()) == common::Status::kOk);
    CHECK(!active.stopping());
    REQUIRE(active.post([]() noexcept { ran.fetch_add(1, std::memory_order_relaxed); }) == common::Status::kOk);
    CHECK(active.stop() == common::Status::kOk);
    CHECK(ran.load(std::memory_order_relaxed) == 11);
}

TEST_CASE(active_object_stop_cancel_discards_queued) {
    static common::ActiveObject<16U> active{};
    static std::atomic<std::int32_t> ran{0};
    static std::atomic<bool> blocking_started{false};
    REQUIRE(active.start(object_config()) == common::Status::kOk);
    // The first command returns once stop() has begun; then the thread exits without the others.
    REQUIRE(active.post([]() noexcept {
        blocking_started.store(true, std::memory_order_release);
        for (std::int32_t i{0}; (i < 100'000) && (!active.stopping()); ++i) {
            std::this_thread::sleep_for(100us);
        }
        ran.fetch_add(1, std::memory_order_relaxed);
    }) == common::Status::kOk);
    REQUIRE(eventually([]() noexcept { return blocking_started.load(std::memory_order_acquire); }));
    for (std::int32_t i{0}; i < 3; ++i) {
        REQUIRE(active.post([]() noexcept { ran.fetch_add(1, std::memory_order_relaxed); }) == common::Status::kOk);
    }
    CHECK(active.stop(common::StopMode::kCancel) == common::Status::kOk);
    CHECK(ran.load(std::memory_order_relaxed) == 1);
    CHECK(active.discarded() == 3U);
    CHECK(active.executed() == 1U);
}

TEST_CASE(active_object_stop_without_a_thread_discards) {
    common::ActiveObject<4U> active{};
    std::int32_t ran{0};
    REQUIRE(active.post([&ran]() noexcept { ++ran; }) == common::Status::kOk);
    CHECK(active.stop() == common::Status::kNotRunning);
    CHECK(ran == 0);  // Never run on the calling thread.
    CHECK(active.discarded() == 1U);
}

TEST_CASE(active_object_posts_racing_stop) {
    // Every post that returned kOk runs (kDrain), however it overlaps stop().
    static common::ActiveObject<32U> active{};
    static std::int64_t executed_commands{0};  // Touched by the object's commands only.
    REQUIRE(active.start(object_config()) == common::Status::kOk);
    constexpr std::size_t kThreads{4U};
    std::array<std::int64_t, kThreads> accepted{};
    std::array<std::thread, kThreads> producers{};
    for (std::size_t t{0U}; t < kThreads; ++t) {
        producers[t] = std::thread{[t, &accepted]() {
            bool stopped{false};
            const common::Deadline deadline{common::Deadline::after(10s)};
            for (std::int32_t i{0}; (i < 100'000'000) && (!stopped) && (!deadline.expired()); ++i) {
                const common::Status status{active.post([]() noexcept { ++executed_commands; })};
                if (status == common::Status::kOk) {
                    ++accepted[t];
                } else if (status == common::Status::kNotRunning) {
                    stopped = true;
                } else {
                    std::this_thread::yield();
                }
            }
        }};
    }
    std::this_thread::sleep_for(20ms);
    CHECK(active.stop(common::StopMode::kDrain) == common::Status::kOk);
    for (std::thread& producer : producers) {
        producer.join();
    }
    std::int64_t total{0};
    for (const std::int64_t count : accepted) {
        total += count;
    }
    CHECK(total > 0);
    CHECK(executed_commands == total);
    CHECK(active.discarded() == 0U);
}

TEST_CASE(active_object_request_stop_from_a_command) {
    static common::ActiveObject<8U> active{};
    REQUIRE(active.start(object_config()) == common::Status::kOk);
    REQUIRE(active.post([]() noexcept { active.request_stop(); }) == common::Status::kOk);
    CHECK(eventually([]() noexcept { return active.stopping(); }));
    CHECK(active.post([]() noexcept {}) == common::Status::kNotRunning);
    CHECK(active.running());  // Until stop() joins the thread.
    CHECK(active.stop() == common::Status::kOk);
    CHECK(!active.running());
}

TEST_CASE(active_object_poll_hook_drives_a_timer_wheel) {
    struct Context final {
        common::TimerWheel<8U, 16U> wheel{1ms, common::MonotonicClock::now()};
        std::atomic<std::int32_t> polls{0};
        std::atomic<bool> fired{false};
    };
    static Context context{};
    static common::ActiveObject<8U> active{};
    REQUIRE(active.post([]() noexcept {
        // Runs on the object's thread, which owns the wheel.
        static_cast<void>(context.wheel.schedule(
            common::MonotonicClock::now() + 5ms,
            [](common::TimerId, void* const self) noexcept {
                static_cast<Context*>(self)->fired.store(true, std::memory_order_release);
            },
            &context));
    }) == common::Status::kOk);
    REQUIRE(active.start(object_config(), 1ms,
                         [](const common::TimePoint now, void* const self) noexcept {
                             Context* const typed{static_cast<Context*>(self)};
                             typed->polls.fetch_add(1, std::memory_order_relaxed);
                             static_cast<void>(typed->wheel.advance(now, 8U));
                         },
                         &context) == common::Status::kOk);
    CHECK(eventually([]() noexcept { return context.fired.load(std::memory_order_acquire); }));
    CHECK(eventually([]() noexcept { return context.polls.load(std::memory_order_relaxed) >= 3; }));
    CHECK(active.heartbeat().last_beat() != common::TimePoint::max());
    CHECK(active.stop() == common::Status::kOk);
}

TEST_CASE(active_object_busy_polling_and_full_commands) {
    static common::ActiveObject<8U> active{};
    REQUIRE(active.start(object_config(), common::Duration::zero()) == common::Status::kOk);
    CHECK(active.start(object_config(), -1ms) == common::Status::kAlreadyRunning);
    // A command whose captures fill the whole inline storage.
    std::array<std::int64_t, common::kInplaceFunctionCapacity / sizeof(std::int64_t)> values{};
    values.fill(3);
    static std::atomic<std::int64_t> sum{0};
    REQUIRE(active.post([values]() noexcept {
        std::int64_t local{0};
        for (const std::int64_t value : values) {
            local += value;
        }
        sum.store(local, std::memory_order_relaxed);
    }) == common::Status::kOk);
    CHECK(eventually([]() noexcept { return sum.load(std::memory_order_relaxed) != 0; }));
    CHECK(sum.load(std::memory_order_relaxed) == static_cast<std::int64_t>(3U * values.size()));
    CHECK(active.stop() == common::Status::kOk);

    common::ActiveObject<4U> stopped{};
    CHECK(stopped.start(object_config(), -1ms) == common::Status::kInvalidArgument);
}
