/// @file rt_periodic_loop.cpp
/// @brief Reference structure of a real-time application built on this library:
///
///   1. Initialisation phase (may allocate, may block): lock memory, configure the allocator,
///      create statically sized channels, start threads with explicit scheduling and affinity.
///   2. Real-time phase: a 1 kHz cyclic loop that only uses bounded, allocation-free,
///      non-blocking operations and publishes samples through a lock-free queue.
///   3. A non-real-time consumer (here: main) drains the queue and does the slow work (I/O).
///      It also supervises the RT loop through a heartbeat and a watchdog.
///
/// Usage: rt_periodic_loop [cpu] [cycles]
/// For SCHED_FIFO run as root, with CAP_SYS_NICE, or with an rtprio limit (see README).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "common/common.hpp"

namespace {

using namespace std::chrono_literals;

struct Sample final {
    std::uint64_t cycle{0U};
    std::int64_t wakeup_latency_ns{0};
    double filtered{0.0};
};

struct RtContext final {
    std::uint32_t cycles{0U};
    std::atomic<bool> finished{false};
    common::Heartbeat alive{};  // Beaten by the RT loop every cycle, checked by main.
    std::uint64_t dropped{0U};  // Written by the RT thread only, read after join.
    bool fp_fault{false};       // Written by the RT thread only, read after join.
    common::LatencyHistogram<1000U, 1000> latency{};  // 1 us buckets up to 1 ms.
};

// Static storage: no allocation, no large objects on any stack.
common::SpscQueue<Sample, 4096U> g_samples{};
RtContext g_context{};

void rt_loop(void* const arg) noexcept {
    RtContext* const context{static_cast<RtContext*>(arg)};

    common::prefault_stack<64U * 1024U>();
    static_cast<void>(common::enable_flush_denormals());
    common::clear_fp_exceptions();

    common::PeriodicTimer timer{1ms};
    common::LowPassFilter<double> filter{0.1};
    timer.start();
    for (std::uint32_t cycle{0U}; cycle < context->cycles; ++cycle) {
        const common::Status status{timer.wait()};
        const common::TimePoint now{common::MonotonicClock::now()};
        context->alive.beat(now);
        const common::Duration latency{now - timer.activation_time()};
        context->latency.record(latency);
        static_cast<void>(status);  // Overruns are visible through timer.missed_count().

        // Bounded "work": first-order low-pass filter on the latency signal.
        const double filtered{filter.update(static_cast<double>(latency.count()))};

        const Sample sample{cycle, latency.count(), filtered};
        if (!g_samples.try_push(sample)) {
            ++context->dropped;  // Never block the RT loop on a slow consumer.
        }
    }

    // Sticky flags: one check covers every FP operation since clear_fp_exceptions().
    context->fp_fault = common::test_fp_exceptions().any_error();
    context->finished.store(true, std::memory_order_release);
}

const char* policy_name(const common::SchedPolicy policy) noexcept {
    return (policy == common::SchedPolicy::kFifo) ? "SCHED_FIFO" : "SCHED_OTHER";
}

}  // namespace

int main(const int argc, const char* const argv[]) {
    const std::size_t cpu{(argc > 1) ? std::size_t{std::strtoul(argv[1], nullptr, 10)} : 1U};
    g_context.cycles = (argc > 2) ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 2000U;

    // ---- Initialisation phase ------------------------------------------------------------
    common::Status status{common::configure_allocator_for_rt()};
    std::printf("allocator config : %s\n", common::to_string(status));
    status = common::lock_process_memory();
    std::printf("mlockall         : %s\n", common::to_string(status));

    common::ThreadConfig config{};
    config.name = "rt_loop";
    config.policy = common::SchedPolicy::kFifo;
    config.priority = 80;
    config.affinity = common::CpuSet::single(cpu);
    config.stack_size = 512U * 1024U;

    // Supervision, registered before the thread starts: the loop must beat at least every 50 ms
    // (50 missed cycles).
    common::Watchdog<1U> watchdog{};
    const common::Result<std::size_t> rt_entry{watchdog.add(g_context.alive, 50ms, "rt_loop")};
    if (!rt_entry.has_value()) {
        std::printf("failed to supervise RT thread: %s\n", common::to_string(rt_entry.status()));
        return EXIT_FAILURE;
    }

    common::RtThread rt_thread{};
    status = rt_thread.start(config, &rt_loop, &g_context);
    if (status == common::Status::kPermissionDenied) {
        std::printf("SCHED_FIFO denied, falling back to SCHED_OTHER (latency will not be bounded)\n");
        config.policy = common::SchedPolicy::kOther;
        config.priority = 0;
        status = rt_thread.start(config, &rt_loop, &g_context);
    }
    if (!common::is_ok(status)) {
        std::printf("failed to start RT thread: %s\n", common::to_string(status));
        return EXIT_FAILURE;
    }
    std::printf("RT thread        : %s prio %d on CPU %zu, %u cycles @ 1 kHz\n", policy_name(config.policy),
                config.priority, cpu, g_context.cycles);

    // ---- Non-RT consumer -----------------------------------------------------------------
    Sample sample{};
    std::uint64_t received{0U};
    bool draining{true};
    while (draining) {
        const bool finished{g_context.finished.load(std::memory_order_acquire)};
        if (!finished) {
            static_cast<void>(watchdog.check());
        }
        while (g_samples.try_pop(sample)) {
            ++received;
            if ((sample.cycle % 500U) == 0U) {
                std::printf("  cycle %5llu  latency %7lld ns  filtered %9.1f ns\n",
                            static_cast<unsigned long long>(sample.cycle),
                            static_cast<long long>(sample.wakeup_latency_ns), sample.filtered);
            }
        }
        draining = !finished;
        if (draining) {
            static_cast<void>(common::sleep_for(10ms));
        }
    }
    status = rt_thread.join();
    static_cast<void>(status);

    const auto& latency{g_context.latency};
    std::printf("\nwake-up latency over %llu cycles (received %llu, dropped %llu)\n",
                static_cast<unsigned long long>(latency.count()), static_cast<unsigned long long>(received),
                static_cast<unsigned long long>(g_context.dropped));
    std::printf("  min %lld ns  mean %.0f ns  p99 <= %lld ns  p99.9 <= %lld ns  max %lld ns\n",
                static_cast<long long>(latency.min_ns()), latency.mean_ns(),
                static_cast<long long>(latency.percentile_ns(0.99)),
                static_cast<long long>(latency.percentile_ns(0.999)), static_cast<long long>(latency.max_ns()));
    std::printf("  watchdog expirations: %llu\n",
                static_cast<unsigned long long>(watchdog.expirations(rt_entry.value())));
    std::printf("  floating-point faults: %s\n", g_context.fp_fault ? "YES" : "none");
    return g_context.fp_fault ? EXIT_FAILURE : EXIT_SUCCESS;
}
