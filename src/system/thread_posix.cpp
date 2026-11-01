#include "common/system/thread.hpp"

#include <pthread.h>
#include <sched.h>
#if defined(__ANDROID__)
#include <unistd.h>  // gettid
#endif

#include <cerrno>
#include <cstring>

#include "detail/posix_status.hpp"

namespace common {
namespace {

#if !defined(__APPLE__)
static_assert(CpuSet::kMaxCpus <= static_cast<std::size_t>(CPU_SETSIZE), "CpuSet exceeds cpu_set_t");
#endif

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

#if defined(__APPLE__)
// macOS and iOS give every policy the priorities 15..47 around a default of 31, and their SCHED_FIFO
// and SCHED_RR are not Linux's real-time classes. As on Windows, priorities are checked against the
// Linux ranges, so that a configuration is equally valid everywhere; kOther runs at the default
// priority, and the real-time policies are kNotSupported.
constexpr std::int32_t kMinRealTimePriority{1};
constexpr std::int32_t kMaxRealTimePriority{99};

Status validate_priority(const SchedPolicy policy, const std::int32_t priority) noexcept {
    bool valid{false};
    switch (policy) {
        case SchedPolicy::kOther:
            valid = (priority == 0);
            break;
        case SchedPolicy::kFifo:
        case SchedPolicy::kRoundRobin:
            valid = (priority >= kMinRealTimePriority) && (priority <= kMaxRealTimePriority);
            break;
    }
    return valid ? Status::kOk : Status::kInvalidArgument;
}

/// @return kOk for kOther; kNotSupported for the real-time policies.
Status check_supported(const SchedPolicy policy) noexcept {
    return (policy == SchedPolicy::kOther) ? Status::kOk : Status::kNotSupported;
}
#else
Status validate_priority(const SchedPolicy policy, const std::int32_t priority) noexcept {
    const int native{to_native(policy)};
    const int min{::sched_get_priority_min(native)};
    const int max{::sched_get_priority_max(native)};
    return ((priority >= min) && (priority <= max)) ? Status::kOk : Status::kInvalidArgument;
}

/// @return kOk: Linux has every policy.
Status check_supported(const SchedPolicy /*policy*/) noexcept {
    return Status::kOk;
}
#endif

#if !defined(__APPLE__)
void to_native(const CpuSet& cpus, cpu_set_t& native) noexcept {
    CPU_ZERO(&native);
    for (std::size_t cpu{0U}; cpu < CpuSet::kMaxCpus; ++cpu) {
        if (cpus.contains(cpu)) {
            CPU_SET(cpu, &native);
        }
    }
}
#endif

/// Names the calling thread; @p name has at most 15 characters.
int set_own_name(const char* const name) noexcept {
#if defined(__APPLE__)
    return ::pthread_setname_np(name);  // macOS and iOS name the calling thread only.
#else
    return ::pthread_setname_np(::pthread_self(), name);
#endif
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

#if defined(__APPLE__)
/// Reads the default priority, which pthread_attr_init() puts into new attributes.
Status default_priority(sched_param& param) noexcept {
    ThreadAttributes attributes{};
    return attributes.valid() ? detail::status_from_errno(::pthread_attr_getschedparam(attributes.get(), &param))
                              : Status::kSystemError;
}
#endif

}  // namespace

Status set_current_thread_scheduling(const SchedPolicy policy, const std::int32_t priority) noexcept {
    Status status{validate_priority(policy, priority)};
    if (is_ok(status)) {
        status = check_supported(policy);
    }
    sched_param param{};
#if defined(__APPLE__)
    if (is_ok(status)) {
        status = default_priority(param);  // kOther runs at the default priority.
    }
#else
    param.sched_priority = priority;
#endif
    if (is_ok(status)) {
        status = detail::status_from_errno(::pthread_setschedparam(::pthread_self(), to_native(policy), &param));
    }
    return status;
}

Status get_current_thread_scheduling(SchedPolicy& policy, std::int32_t& priority) noexcept {
    int native{SCHED_OTHER};
    sched_param param{};
    Status status{detail::status_from_errno(::pthread_getschedparam(::pthread_self(), &native, &param))};
#if defined(__APPLE__)
    // Relative to the default priority, as Windows reports it: 0 unless changed otherwise.
    sched_param standard{};
    if (is_ok(status)) {
        status = default_priority(standard);
    }
#endif
    if (is_ok(status)) {
#if defined(__APPLE__)
        priority = param.sched_priority - standard.sched_priority;
#else
        priority = param.sched_priority;
#endif
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
#if defined(__APPLE__)
        status = Status::kNotSupported;  // macOS and iOS do not bind threads to CPUs.
#elif defined(__ANDROID__)
        cpu_set_t native{};
        to_native(cpus, native);
        // Bionic has no pthread_setaffinity_np: the thread's own ID selects it.
        status = (::sched_setaffinity(::gettid(), sizeof(native), &native) == 0) ? Status::kOk
                                                                                 : detail::status_from_errno(errno);
#else
        cpu_set_t native{};
        to_native(cpus, native);
        status = detail::status_from_errno(::pthread_setaffinity_np(::pthread_self(), sizeof(native), &native));
#endif
    }
    return status;
}

Status set_current_thread_name(const char* const name) noexcept {
    Status status{Status::kInvalidArgument};
    if ((name != nullptr) && (std::strlen(name) <= 15U)) {
        status = detail::status_from_errno(set_own_name(name));
    }
    return status;
}

std::int32_t current_cpu() noexcept {
#if defined(__APPLE__)
    return -1;  // macOS and iOS do not tell.
#else
    return ::sched_getcpu();
#endif
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
    if (is_ok(status)) {
        status = check_supported(config.policy);
    }
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
#if !defined(__APPLE__)  // On macOS and iOS, new attributes hold the default priority, at which kOther runs.
    if (rc == 0) {
        sched_param param{};
        param.sched_priority = config.priority;
        rc = ::pthread_attr_setschedparam(attr, &param);
    }
#endif
    if ((rc == 0) && (config.stack_size != 0U)) {
        rc = ::pthread_attr_setstacksize(attr, config.stack_size);
    }
#if defined(__ANDROID__) || defined(__APPLE__)
    // Bionic cannot create a thread with an affinity (no pthread_attr_setaffinity_np), and macOS and
    // iOS do not bind threads to CPUs at all.
    if ((rc == 0) && (!config.affinity.empty())) {
        return Status::kNotSupported;
    }
#else
    if ((rc == 0) && (!config.affinity.empty())) {
        cpu_set_t cpus{};
        to_native(config.affinity, cpus);
        rc = ::pthread_attr_setaffinity_np(attr, sizeof(cpus), &cpus);
    }
#endif
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
        static_cast<void>(set_own_name(thread->name_.data()));
    }
    thread->entry_(thread->context_);
    return nullptr;
}

}  // namespace common
