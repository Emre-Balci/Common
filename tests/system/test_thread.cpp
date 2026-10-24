#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

#include <pthread.h>

#include "common/core/status.hpp"
#include "common/system/memory.hpp"
#include "common/system/thread.hpp"
#include "test_framework.hpp"

namespace {

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
    static_cast<void>(::pthread_getname_np(::pthread_self(), probe->name.data(), probe->name.size()));
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
    REQUIRE(common::is_ok(thread.start(config, &probe_entry, &probe)));
    CHECK(thread.joinable());
    CHECK(thread.start(config, &probe_entry, &probe) == common::Status::kAlreadyRunning);
    CHECK(common::is_ok(thread.join()));
    CHECK(!thread.joinable());
    CHECK(thread.join() == common::Status::kNotRunning);

    CHECK(probe.ran.load());
    CHECK(probe.cpu.load() == 0);
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
    // Unprivileged environments (CI, WSL without rtprio limits) must report, not silently ignore.
    CHECK((status == common::Status::kOk) || (status == common::Status::kPermissionDenied));
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
        CHECK(common::is_ok(common::set_current_thread_affinity(common::CpuSet::single(0U))));
        CHECK(common::current_cpu() == 0);
        CHECK(common::set_current_thread_affinity(common::CpuSet{}) == common::Status::kInvalidArgument);
        CHECK(common::is_ok(common::set_current_thread_scheduling(common::SchedPolicy::kOther, 0)));
    }};
    worker.join();
}
