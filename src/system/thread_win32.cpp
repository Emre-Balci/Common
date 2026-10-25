#include "common/system/thread.hpp"

#include <process.h>  // _beginthreadex

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "detail/win32.hpp"
#include "detail/win32_status.hpp"

namespace common {
namespace {

// Windows has no SCHED_FIFO or SCHED_RR. Their priorities are still checked against the Linux range,
// so that a configuration is equally valid on both platforms.
constexpr std::int32_t kMinRealTimePriority{1};
constexpr std::int32_t kMaxRealTimePriority{99};

// The longest name pthread_setname_np() accepts on Linux; the same limit applies here.
constexpr std::size_t kMaxThreadNameLength{15U};

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

/// @return kOk for kOther; kNotSupported for the real-time policies, which Windows does not have.
Status check_supported(const SchedPolicy policy) noexcept {
    return (policy == SchedPolicy::kOther) ? Status::kOk : Status::kNotSupported;
}

/// Finds the processor group and the mask within it that @p cpus select. CPUs are numbered across
/// the groups in order, as current_cpu() numbers them; CPUs that do not exist are ignored, as Linux
/// does.
/// @return kInvalidArgument if no CPU of the set exists, kNotSupported if the CPUs lie in more than
///         one group (a thread runs in one group only).
Status to_group_affinity(const CpuSet& cpus, GROUP_AFFINITY& affinity) noexcept {
    affinity = GROUP_AFFINITY{};
    const WORD groups{::GetActiveProcessorGroupCount()};
    Status status{Status::kInvalidArgument};  // Until a CPU of the set is found.
    std::size_t first{0U};                     // Number of the first CPU of the current group.
    for (WORD group{0U}; (group < groups) && (status != Status::kNotSupported) && (first < CpuSet::kMaxCpus);
         ++group) {
        const DWORD count{::GetActiveProcessorCount(group)};
        KAFFINITY mask{0U};
        // A group has at most 64 CPUs, one bit of KAFFINITY each.
        for (DWORD index{0U}; (index < count) && (index < 64U) && ((first + index) < CpuSet::kMaxCpus); ++index) {
            if (cpus.contains(first + index)) {
                mask |= KAFFINITY{1U} << index;
            }
        }
        if ((mask != 0U) && is_ok(status)) {
            status = Status::kNotSupported;  // A second group with CPUs of the set.
        } else if (mask != 0U) {
            affinity.Group = group;
            affinity.Mask = mask;
            status = Status::kOk;
        } else {
            // No CPU of the set in this group.
        }
        first += count;
    }
    return status;
}

/// Maps the errno value of a failed _beginthreadex().
Status thread_creation_status(const int error) noexcept {
    Status status{Status::kSystemError};
    if ((error == EAGAIN) || (error == EACCES)) {
        status = Status::kResourceLimit;  // Too many threads, or not enough memory.
    } else if (error == EINVAL) {
        status = Status::kInvalidArgument;
    } else {
        // Not documented for _beginthreadex(): keep kSystemError.
    }
    return status;
}

/// Sets the description of @p thread, which debuggers and profilers show as its name.
/// @pre name has at most kMaxThreadNameLength bytes
Status set_thread_description(const HANDLE thread, const char* const name) noexcept {
    // UTF-8 never needs more UTF-16 code units than bytes, so a name of up to 15 bytes and its NUL fit.
    std::array<wchar_t, kMaxThreadNameLength + 1U> wide{};
    const int converted{
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide.data(), static_cast<int>(wide.size()))};
    Status status{(converted > 0) ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status)) {
        status = SUCCEEDED(::SetThreadDescription(thread, wide.data())) ? Status::kOk : Status::kSystemError;
    }
    return status;
}

}  // namespace

Status set_current_thread_scheduling(const SchedPolicy policy, const std::int32_t priority) noexcept {
    Status status{validate_priority(policy, priority)};
    if (is_ok(status)) {
        status = check_supported(policy);
    }
    if (is_ok(status)) {
        status = (::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_NORMAL) != FALSE) ? Status::kOk
                                                                                             : detail::last_win32_error();
    }
    return status;
}

Status get_current_thread_scheduling(SchedPolicy& policy, std::int32_t& priority) noexcept {
    const int native{::GetThreadPriority(::GetCurrentThread())};
    Status status{(native == THREAD_PRIORITY_ERROR_RETURN) ? detail::last_win32_error() : Status::kOk};
    if (is_ok(status)) {
        policy = SchedPolicy::kOther;
        priority = native;
    }
    return status;
}

Status set_current_thread_affinity(const CpuSet& cpus) noexcept {
    GROUP_AFFINITY affinity{};
    Status status{to_group_affinity(cpus, affinity)};
    if (is_ok(status)) {
        status = (::SetThreadGroupAffinity(::GetCurrentThread(), &affinity, nullptr) != FALSE)
                     ? Status::kOk
                     : detail::last_win32_error();
    }
    return status;
}

Status set_current_thread_name(const char* const name) noexcept {
    Status status{Status::kInvalidArgument};
    if ((name != nullptr) && (std::strlen(name) <= kMaxThreadNameLength)) {
        status = set_thread_description(::GetCurrentThread(), name);
    }
    return status;
}

std::int32_t current_cpu() noexcept {
    PROCESSOR_NUMBER number{};
    ::GetCurrentProcessorNumberEx(&number);
    // The CPUs of the groups before this one come first.
    DWORD cpu{number.Number};
    for (WORD group{0U}; group < number.Group; ++group) {
        cpu += ::GetActiveProcessorCount(group);
    }
    return static_cast<std::int32_t>(cpu);
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
    if (is_ok(status) && (config.stack_size > std::numeric_limits<unsigned>::max())) {
        status = Status::kInvalidArgument;
    }
    GROUP_AFFINITY affinity{};
    if (is_ok(status) && (!config.affinity.empty())) {
        status = to_group_affinity(config.affinity, affinity);
    }
    if (!is_ok(status)) {
        return status;
    }

    entry_ = entry;
    context_ = context;
    // Created suspended, so that affinity and priority are in place before the thread runs. The stack
    // size is the size to reserve, as with pthread_attr_setstacksize(); 0 is the default of the image.
    const std::uintptr_t created{::_beginthreadex(nullptr, static_cast<unsigned>(config.stack_size),
                                                  &RtThread::trampoline, this,
                                                  CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)};
    if (created == 0U) {
        return thread_creation_status(errno);
    }
    // Deviation (MISRA C++:2023 Rule 8.2.5): _beginthreadex() returns the thread's HANDLE as an integer.
    const HANDLE thread{reinterpret_cast<HANDLE>(created)};

    if ((!config.affinity.empty()) && (::SetThreadGroupAffinity(thread, &affinity, nullptr) == FALSE)) {
        status = detail::last_win32_error();
    }
    if (is_ok(status) && (::SetThreadPriority(thread, THREAD_PRIORITY_NORMAL) == FALSE)) {
        status = detail::last_win32_error();
    }
    if (is_ok(status) && (name_[0U] != '\0')) {
        // Best effort, as on Linux: a thread without a name works the same.
        static_cast<void>(set_thread_description(thread, name_.data()));
    }
    if (!is_ok(status)) {
        entry_ = nullptr;  // The thread then returns at once: it never runs the entry point.
    }
    if (::ResumeThread(thread) == static_cast<DWORD>(-1)) {
        // Cannot happen for a thread that this process just created suspended; the thread never ran,
        // so it holds no locks and terminating it is safe.
        status = detail::last_win32_error();
        static_cast<void>(::TerminateThread(thread, 0U));
    }
    if (is_ok(status)) {
        handle_ = thread;
        running_ = true;
    } else {
        static_cast<void>(::WaitForSingleObject(thread, INFINITE));
        static_cast<void>(::CloseHandle(thread));
    }
    return status;
}

Status RtThread::join() noexcept {
    Status status{Status::kNotRunning};
    if (running_) {
        if (::GetThreadId(handle_) == ::GetCurrentThreadId()) {
            status = Status::kSystemError;  // Joining itself would wait forever (EDEADLK on Linux).
        } else {
            status = (::WaitForSingleObject(handle_, INFINITE) == WAIT_OBJECT_0) ? Status::kOk
                                                                                : detail::last_win32_error();
        }
        static_cast<void>(::CloseHandle(handle_));
        handle_ = nullptr;
        running_ = false;
    }
    return status;
}

unsigned __stdcall RtThread::trampoline(void* const self) noexcept {
    const RtThread* const thread{static_cast<const RtThread*>(self)};
    if (thread->entry_ != nullptr) {
        thread->entry_(thread->context_);
    }
    return 0U;
}

}  // namespace common
