#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

#if !defined(_WIN32)
#include <pthread.h>
#endif

#include "common/core/status.hpp"
#include "common/system/memory.hpp"
#include "common/system/thread.hpp"
#include "test_framework.hpp"
#if defined(_WIN32)
#include "test_platform.hpp"
#endif

namespace {

/// The calling thread's name (on Windows, its description), as set by RtThread or
/// set_current_thread_name().
void read_thread_name(std::array<char, 16U>& name) {
#if defined(_WIN32)
    PWSTR description{nullptr};
    if (SUCCEEDED(::GetThreadDescription(::GetCurrentThread(), &description))) {
        static_cast<void>(::WideCharToMultiByte(CP_UTF8, 0U, description, -1, name.data(), static_cast<int>(name.size()),
                                                nullptr, nullptr));
        static_cast<void>(::LocalFree(description));
    }
#else
    static_cast<void>(::pthread_getname_np(::pthread_self(), name.data(), name.size()));
#endif
}

struct ThreadProbe final {
    std::atomic<bool> ran{false};
    std::atomic<std::int32_t> cpu{-1};
    common::SchedPolicy policy{common::SchedPolicy::kOther};
    std::int32_t priority{-1};
    std::array<char, 16U> name{};
};

void probe_entry(void* const context) noexcept {
    ThreadProbe* const probe{static_cast<ThreadProbe*>(context)};
    common::prefault_stack<16U * 1024U>();
    probe->cpu.store(common::current_cpu());
    static_cast<void>(common::get_current_thread_scheduling(probe->policy, probe->priority));
    read_thread_name(probe->name);
    probe->ran.store(true);
}

}  // namespace

TEST_CASE(cpu_set_operations) {
    common::CpuSet set{};
    CHECK(set.empty());
    CHECK(common::is_ok(set.add(3U)));
    CHECK(set.add(common::CpuSet::kMaxCpus) == common::Status::kInvalidArgument);
    CHECK(set.contains(3U));
    CHECK(!set.contains(2U));
    CHECK(set.count() == 1U);
    set.remove(3U);
    CHECK(set.empty());
    CHECK(common::CpuSet::single(0U).contains(0U));
}

TEST_CASE(rt_thread_normal_policy_with_affinity) {
    ThreadProbe probe{};
    common::ThreadConfig config{};
    config.name = "probe";
    config.policy = common::SchedPolicy::kOther;
    config.priority = 0;
    config.affinity = common::CpuSet::single(0U);
    config.stack_size = 256U * 1024U;

    common::RtThread thread{};
#if defined(__ANDROID__) || defined(__APPLE__)
    // Bionic cannot create a thread with an affinity (the thread sets its own instead), and macOS and
    // iOS have none.
    CHECK(thread.start(config, &probe_entry, &probe) == common::Status::kNotSupported);
    CHECK(!thread.joinable());
    config.affinity = common::CpuSet{};
#endif
    REQUIRE(common::is_ok(thread.start(config, &probe_entry, &probe)));
    CHECK(thread.joinable());
    CHECK(thread.start(config, &probe_entry, &probe) == common::Status::kAlreadyRunning);
    CHECK(common::is_ok(thread.join()));
    CHECK(!thread.joinable());
    CHECK(thread.join() == common::Status::kNotRunning);

    CHECK(probe.ran.load());
#if !defined(__ANDROID__) && !defined(__APPLE__)
    CHECK(probe.cpu.load() == 0);
#endif
    CHECK(probe.policy == common::SchedPolicy::kOther);
    CHECK(std::strcmp(probe.name.data(), "probe") == 0);
}

TEST_CASE(rt_thread_fifo_applied_or_denied) {
    ThreadProbe probe{};
    common::ThreadConfig config{};
    config.name = "fifo-probe";
    config.policy = common::SchedPolicy::kFifo;
    config.priority = 10;

    common::RtThread thread{};
    const common::Status status{thread.start(config, &probe_entry, &probe)};
#if defined(_WIN32) || defined(__APPLE__)
    CHECK(status == common::Status::kNotSupported);  // Windows, macOS and iOS have no real-time policies.
#else
    // Unprivileged environments (CI, WSL without rtprio limits) must report, not silently ignore.
    CHECK((status == common::Status::kOk) || (status == common::Status::kPermissionDenied));
#endif
    if (common::is_ok(status)) {
        CHECK(common::is_ok(thread.join()));
        CHECK(probe.policy == common::SchedPolicy::kFifo);
        CHECK(probe.priority == 10);
    }
}

TEST_CASE(rt_thread_rejects_invalid_config) {
    ThreadProbe probe{};
    common::RtThread thread{};
    common::ThreadConfig config{};
    config.policy = common::SchedPolicy::kFifo;
    config.priority = 0;  // FIFO requires 1..99.
    CHECK(thread.start(config, &probe_entry, &probe) == common::Status::kInvalidArgument);
    config.policy = common::SchedPolicy::kOther;
    config.name = "name-longer-than-15";
    CHECK(thread.start(config, &probe_entry, &probe) == common::Status::kInvalidArgument);
    config.name = nullptr;
    CHECK(thread.start(config, nullptr, &probe) == common::Status::kInvalidArgument);
    CHECK(!thread.joinable());
}

TEST_CASE(current_thread_configuration) {
    std::thread worker{[]() {
        CHECK(common::is_ok(common::set_current_thread_name("worker")));
        CHECK(common::set_current_thread_name("this-name-is-too-long") == common::Status::kInvalidArgument);
        CHECK(common::set_current_thread_affinity(common::CpuSet{}) == common::Status::kInvalidArgument);
#if defined(__APPLE__)
        // macOS and iOS do not bind threads to CPUs, nor tell where one runs.
        CHECK(common::set_current_thread_affinity(common::CpuSet::single(0U)) == common::Status::kNotSupported);
        CHECK(common::current_cpu() == -1);
#else
        CHECK(common::is_ok(common::set_current_thread_affinity(common::CpuSet::single(0U))));
        CHECK(common::current_cpu() == 0);
        // No CPU of the set exists.
        CHECK(common::set_current_thread_affinity(common::CpuSet::single(common::CpuSet::kMaxCpus - 1U)) ==
              common::Status::kInvalidArgument);
#endif
        CHECK(common::is_ok(common::set_current_thread_scheduling(common::SchedPolicy::kOther, 0)));
        common::SchedPolicy policy{common::SchedPolicy::kFifo};
        std::int32_t priority{-1};
        CHECK(common::is_ok(common::get_current_thread_scheduling(policy, priority)));
        CHECK(policy == common::SchedPolicy::kOther);
        CHECK(priority == 0);
#if defined(_WIN32) || defined(__APPLE__)
        CHECK(common::set_current_thread_scheduling(common::SchedPolicy::kRoundRobin, 10) ==
              common::Status::kNotSupported);
#endif
    }};
    worker.join();
}
