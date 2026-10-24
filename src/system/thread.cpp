#include "common/system/thread.hpp"

#include <pthread.h>
#include <sched.h>

#include <cstring>

#include "common/core/assert.hpp"
#include "detail/posix_status.hpp"

namespace common {
namespace {

static_assert(CpuSet::kMaxCpus <= static_cast<std::size_t>(CPU_SETSIZE), "CpuSet exceeds cpu_set_t");

int to_native(const SchedPolicy policy) noexcept {
    int native{SCHED_OTHER};
    switch (policy) {
        case SchedPolicy::kOther:
            native = SCHED_OTHER;
            break;
        case SchedPolicy::kFifo:
            native = SCHED_FIFO;
            break;
        case SchedPolicy::kRoundRobin:
            native = SCHED_RR;
            break;
    }
    return native;
}

Status validate_priority(const SchedPolicy policy, const std::int32_t priority) noexcept {
    const int native{to_native(policy)};
    const int min{::sched_get_priority_min(native)};
    const int max{::sched_get_priority_max(native)};
    return ((priority >= min) && (priority <= max)) ? Status::kOk : Status::kInvalidArgument;
}

void to_native(const CpuSet& cpus, cpu_set_t& native) noexcept {
    CPU_ZERO(&native);
    for (std::size_t cpu{0U}; cpu < CpuSet::kMaxCpus; ++cpu) {
        if (cpus.contains(cpu)) {
            CPU_SET(cpu, &native);
        }
    }
}

/// RAII owner of pthread_attr_t.
class ThreadAttributes final {
public:
    ThreadAttributes() noexcept : valid_{::pthread_attr_init(&attr_) == 0} {}
    ~ThreadAttributes() {
        if (valid_) {
            static_cast<void>(::pthread_attr_destroy(&attr_));
        }
    }
    ThreadAttributes(const ThreadAttributes&) = delete;
    ThreadAttributes(ThreadAttributes&&) = delete;
    ThreadAttributes& operator=(const ThreadAttributes&) = delete;
    ThreadAttributes& operator=(ThreadAttributes&&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }
    [[nodiscard]] pthread_attr_t* get() noexcept {
        return &attr_;
    }

private:
    pthread_attr_t attr_{};
    bool valid_;
};

}  // namespace

CpuSet CpuSet::single(const std::size_t cpu) noexcept {
    CpuSet set{};
    static_cast<void>(set.add(cpu));
    return set;
}

Status CpuSet::add(const std::size_t cpu) noexcept {
    Status status{Status::kInvalidArgument};
    if (cpu < kMaxCpus) {
        cpus_[cpu] = true;  // operator[] never throws, unlike bitset::set().
        status = Status::kOk;
    }
    return status;
}

void CpuSet::remove(const std::size_t cpu) noexcept {
    if (cpu < kMaxCpus) {
        cpus_[cpu] = false;
    }
}

bool CpuSet::contains(const std::size_t cpu) const noexcept {
    return (cpu < kMaxCpus) && cpus_[cpu];
}

std::size_t CpuSet::count() const noexcept {
    return cpus_.count();
}

bool CpuSet::empty() const noexcept {
    return cpus_.none();
}

Status set_current_thread_scheduling(const SchedPolicy policy, const std::int32_t priority) noexcept {
    Status status{validate_priority(policy, priority)};
    if (is_ok(status)) {
        sched_param param{};
        param.sched_priority = priority;
        status = detail::status_from_errno(::pthread_setschedparam(::pthread_self(), to_native(policy), &param));
    }
    return status;
}

Status get_current_thread_scheduling(SchedPolicy& policy, std::int32_t& priority) noexcept {
    int native{SCHED_OTHER};
    sched_param param{};
    Status status{detail::status_from_errno(::pthread_getschedparam(::pthread_self(), &native, &param))};
    if (is_ok(status)) {
        priority = param.sched_priority;
        if (native == SCHED_FIFO) {
            policy = SchedPolicy::kFifo;
        } else if (native == SCHED_RR) {
            policy = SchedPolicy::kRoundRobin;
        } else {
            policy = SchedPolicy::kOther;
        }
    }
    return status;
}

Status set_current_thread_affinity(const CpuSet& cpus) noexcept {
    Status status{Status::kInvalidArgument};
    if (!cpus.empty()) {
        cpu_set_t native{};
        to_native(cpus, native);
        status = detail::status_from_errno(::pthread_setaffinity_np(::pthread_self(), sizeof(native), &native));
    }
    return status;
}

Status set_current_thread_name(const char* const name) noexcept {
    Status status{Status::kInvalidArgument};
    if ((name != nullptr) && (std::strlen(name) <= 15U)) {
        status = detail::status_from_errno(::pthread_setname_np(::pthread_self(), name));
    }
    return status;
}

Status apply_to_current_thread(const ThreadConfig& config) noexcept {
    Status status{Status::kOk};
    if (config.name != nullptr) {
        status = set_current_thread_name(config.name);
    }
    if (is_ok(status) && (!config.affinity.empty())) {
        status = set_current_thread_affinity(config.affinity);
    }
    if (is_ok(status)) {
        status = set_current_thread_scheduling(config.policy, config.priority);
    }
    return status;
}

std::int32_t current_cpu() noexcept {
    return ::sched_getcpu();
}

RtThread::~RtThread() {
    COMMON_ASSERT(!running_);
}

Status RtThread::start(const ThreadConfig& config, const Entry entry, void* const context) noexcept {
    if (running_) {
        return Status::kAlreadyRunning;
    }
    if (entry == nullptr) {
        return Status::kInvalidArgument;
    }

    name_.fill('\0');
    if (config.name != nullptr) {
        const std::size_t length{::strnlen(config.name, kMaxNameLength + 1U)};
        if (length > kMaxNameLength) {
            return Status::kInvalidArgument;
        }
        static_cast<void>(std::memcpy(name_.data(), config.name, length));
    }

    Status status{validate_priority(config.policy, config.priority)};
    if (!is_ok(status)) {
        return status;
    }

    ThreadAttributes attributes{};
    if (!attributes.valid()) {
        return Status::kSystemError;
    }
    pthread_attr_t* const attr{attributes.get()};

    // Without EXPLICIT_SCHED the policy below is silently ignored and the creator's is inherited.
    int rc{::pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED)};
    if (rc == 0) {
        rc = ::pthread_attr_setschedpolicy(attr, to_native(config.policy));
    }
    if (rc == 0) {
        sched_param param{};
        param.sched_priority = config.priority;
        rc = ::pthread_attr_setschedparam(attr, &param);
    }
    if ((rc == 0) && (config.stack_size != 0U)) {
        rc = ::pthread_attr_setstacksize(attr, config.stack_size);
    }
    if ((rc == 0) && (!config.affinity.empty())) {
        cpu_set_t cpus{};
        to_native(config.affinity, cpus);
        rc = ::pthread_attr_setaffinity_np(attr, sizeof(cpus), &cpus);
    }
    if (rc != 0) {
        return detail::status_from_errno(rc);
    }

    entry_ = entry;
    context_ = context;
    rc = ::pthread_create(&handle_, attr, &RtThread::trampoline, this);
    status = detail::status_from_errno(rc);
    running_ = is_ok(status);
    return status;
}

Status RtThread::join() noexcept {
    Status status{Status::kNotRunning};
    if (running_) {
        status = detail::status_from_errno(::pthread_join(handle_, nullptr));
        running_ = false;
    }
    return status;
}

void* RtThread::trampoline(void* const self) noexcept {
    const RtThread* const thread{static_cast<const RtThread*>(self)};
    if (thread->name_[0U] != '\0') {
        static_cast<void>(::pthread_setname_np(::pthread_self(), thread->name_.data()));
    }
    thread->entry_(thread->context_);
    return nullptr;
}

}  // namespace common
