#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "common/concurrency/mpmc_queue.hpp"
#include "common/concurrency/spin.hpp"
#include "common/concurrency/spsc_queue.hpp"
#include "test_framework.hpp"

namespace {

constexpr std::uint64_t kStressItems{1'000'000U};

common::SpscQueue<std::uint64_t, 1024U> g_spsc_stress{};
common::MpmcQueue<std::uint64_t, 1024U> g_mpmc_stress{};

}  // namespace

TEST_CASE(spsc_push_pop_fifo) {
    static common::SpscQueue<std::int32_t, 4U> queue{};
    std::int32_t out{0};
    CHECK(queue.empty_approx());
    CHECK(!queue.try_pop(out));
    CHECK(queue.try_push(1));
    CHECK(queue.try_push(2));
    CHECK(queue.try_push(3));
    CHECK(queue.try_push(4));
    CHECK(!queue.try_push(5));  // All four slots usable, fifth rejected.
    CHECK(queue.size_approx() == 4U);
    for (std::int32_t expected{1}; expected <= 4; ++expected) {
        REQUIRE(queue.try_pop(out));
        CHECK(out == expected);
    }
    CHECK(!queue.try_pop(out));
}

TEST_CASE(spsc_wraparound_and_front) {
    static common::SpscQueue<std::int32_t, 2U> queue{};
    for (std::int32_t i{0}; i < 100; ++i) {
        REQUIRE(queue.try_push(i));
        const std::int32_t* const front{queue.front()};
        REQUIRE(front != nullptr);
        CHECK(*front == i);
        queue.pop();
        CHECK(queue.front() == nullptr);
    }
}

TEST_CASE(spsc_threaded_stress) {
    std::thread producer{[]() {
        for (std::uint64_t i{0U}; i < kStressItems; ++i) {
            while (!g_spsc_stress.try_push(i)) {
                common::cpu_relax();
            }
        }
    }};
    std::uint64_t expected{0U};
    std::uint64_t value{0U};
    bool ordered{true};
    while (expected < kStressItems) {
        if (g_spsc_stress.try_pop(value)) {
            ordered = ordered && (value == expected);
            ++expected;
        } else {
            common::cpu_relax();
        }
    }
    producer.join();
    CHECK(ordered);
    CHECK(g_spsc_stress.empty_approx());
}

TEST_CASE(mpmc_push_pop_full_empty) {
    static common::MpmcQueue<std::int32_t, 4U> queue{};
    std::int32_t out{0};
    CHECK(!queue.try_pop(out));
    for (std::int32_t i{0}; i < 4; ++i) {
        CHECK(queue.try_push(i));
    }
    CHECK(!queue.try_push(99));
    for (std::int32_t i{0}; i < 4; ++i) {
        REQUIRE(queue.try_pop(out));
        CHECK(out == i);
    }
    CHECK(!queue.try_pop(out));
    // Second lap exercises sequence-number wrap.
    for (std::int32_t i{0}; i < 4; ++i) {
        CHECK(queue.try_push(i + 10));
    }
    REQUIRE(queue.try_pop(out));
    CHECK(out == 10);
}

TEST_CASE(mpmc_threaded_stress) {
    constexpr std::uint32_t kProducers{4U};
    constexpr std::uint32_t kConsumers{4U};
    constexpr std::uint64_t kPerProducer{kStressItems / kProducers};
    std::atomic<std::uint64_t> consumed{0U};
    std::atomic<std::uint64_t> sum{0U};

    std::vector<std::thread> threads{};
    for (std::uint32_t p{0U}; p < kProducers; ++p) {
        threads.emplace_back([p]() {
            for (std::uint64_t i{0U}; i < kPerProducer; ++i) {
                const std::uint64_t value{(static_cast<std::uint64_t>(p) * kPerProducer) + i + 1U};
                while (!g_mpmc_stress.try_push(value)) {
                    common::cpu_relax();
                }
            }
        });
    }
    for (std::uint32_t c{0U}; c < kConsumers; ++c) {
        threads.emplace_back([&consumed, &sum]() {
            std::uint64_t value{0U};
            while (consumed.load(std::memory_order_relaxed) < (kPerProducer * kProducers)) {
                if (g_mpmc_stress.try_pop(value)) {
                    sum.fetch_add(value, std::memory_order_relaxed);
                    consumed.fetch_add(1U, std::memory_order_relaxed);
                } else {
                    common::cpu_relax();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const std::uint64_t n{kPerProducer * kProducers};
    CHECK(consumed.load() == n);
    CHECK(sum.load() == ((n * (n + 1U)) / 2U));
}
