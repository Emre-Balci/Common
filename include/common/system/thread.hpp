/// @file thread.hpp
/// @brief Explicit scheduling policy, priority and CPU affinity for real-time threads.

#ifndef COMMON_SYSTEM_THREAD_HPP
#define COMMON_SYSTEM_THREAD_HPP

#if !defined(_WIN32)
#include <pthread.h>
#endif

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

#include "common/core/status.hpp"

namespace common {

/// Scheduling policy of a thread.
///
/// Windows, macOS and iOS have no real-time policies: there, kOther is the normal thread priority
/// (on macOS and iOS, the default one), and kFifo and kRoundRobin are reported as kNotSupported
/// (after their priority is checked as on Linux, so that a configuration is equally valid on all).
enum class SchedPolicy : std::uint8_t {
    kOther,       ///< SCHED_OTHER (CFS); priority must be 0.
    kFifo,        ///< SCHED_FIFO; priority 1..99.
    kRoundRobin,  ///< SCHED_RR; priority 1..99.
};

/// Set of logical CPUs. An empty set means "do not change affinity".
///
/// On Windows, the CPUs are numbered across the processor groups in order (the CPUs of group 0
/// first), as current_cpu() reports them, and a thread can only run on the CPUs of one group.
class CpuSet final {
public:
    static constexpr std::size_t kMaxCpus{1024U};

    [[nodiscard]] static CpuSet single(std::size_t cpu) noexcept;

    /// @return kInvalidArgument if @p cpu >= kMaxCpus.
    Status add(std::size_t cpu) noexcept;
    void remove(std::size_t cpu) noexcept;
    [[nodiscard]] bool contains(std::size_t cpu) const noexcept;
    [[nodiscard]] std::size_t count() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::bitset<kMaxCpus> cpus_{};
};

struct ThreadConfig final {
    /// Thread name shown by top/ps (at most 15 characters); nullptr keeps the inherited name.
    const char* name{nullptr};
    SchedPolicy policy{SchedPolicy::kFifo};
    std::int32_t priority{50};
    /// CPUs the thread may run on; empty keeps the inherited affinity. On Android, RtThread::start()
    /// reports kNotSupported for a non-empty set (bionic cannot create a thread with an affinity):
    /// the thread calls set_current_thread_affinity() itself. macOS and iOS have no affinity:
    /// kNotSupported there, too.
    CpuSet affinity{};
    /// Stack size in bytes; 0 uses the system default.
    std::size_t stack_size{0U};
};

/// Applies a policy and priority to the calling thread.
/// SCHED_FIFO / SCHED_RR need CAP_SYS_NICE or an RLIMIT_RTPRIO grant (see README); on Windows, macOS
/// and iOS they are kNotSupported, and kOther sets the normal thread priority.
Status set_current_thread_scheduling(SchedPolicy policy, std::int32_t priority) noexcept;

/// Reads the calling thread's policy and priority. On Windows, the policy is always kOther and the
/// priority is the Windows thread priority, 0 (THREAD_PRIORITY_NORMAL) unless changed otherwise. On
/// macOS and iOS, the priority is the distance from the default priority, likewise 0 unless changed.
Status get_current_thread_scheduling(SchedPolicy& policy, std::int32_t& priority) noexcept;

/// Restricts the calling thread to @p cpus. @return kInvalidArgument for an empty set or one without
/// an existing CPU; on Windows kNotSupported for CPUs in more than one processor group; on macOS and
/// iOS, which do not bind threads to CPUs, kNotSupported.
Status set_current_thread_affinity(const CpuSet& cpus) noexcept;

/// Sets the name of the calling thread, at most 15 characters (pthread_setname_np, or
/// SetThreadDescription on Windows, where debuggers and profilers show it).
Status set_current_thread_name(const char* name) noexcept;

/// Applies name, affinity and scheduling (in that order) to the calling thread, e.g. main().
/// Stops at, and returns, the first error. stack_size is ignored.
Status apply_to_current_thread(const ThreadConfig& config) noexcept;

/// @return the CPU the caller is running on, or -1 on error (always on macOS and iOS).
[[nodiscard]] std::int32_t current_cpu() noexcept;

/// pthread wrapper that creates the thread with its scheduling attributes already applied
/// (PTHREAD_EXPLICIT_SCHED), so it never runs with inherited, unintended parameters. On Windows the
/// thread is created suspended and runs only after its affinity, priority and name are set.
///
/// The entry point is a plain function pointer plus context: no std::function, no allocation
/// by this class. Creating a thread is an initialisation-phase operation (the kernel and libc
/// allocate the stack); do it before entering the real-time phase.
///
/// join() must be called before destruction; destroying a running thread is a contract violation.
class RtThread final {
public:
    using Entry = void (*)(void* context) noexcept;

    RtThread() noexcept = default;
    ~RtThread();
    RtThread(const RtThread&) = delete;
    RtThread(RtThread&&) = delete;
    RtThread& operator=(const RtThread&) = delete;
    RtThread& operator=(RtThread&&) = delete;

    /// @return kPermissionDenied if the process may not use the requested real-time policy.
    Status start(const ThreadConfig& config, Entry entry, void* context) noexcept;

    /// Blocks until the thread finishes. Never call from a real-time thread.
    Status join() noexcept;

    [[nodiscard]] bool joinable() const noexcept {
        return running_;
    }

private:
    static constexpr std::size_t kMaxNameLength{15U};

#if defined(_WIN32)
    static unsigned __stdcall trampoline(void* self) noexcept;

    void* handle_{nullptr};  ///< The thread's HANDLE.
#else
    static void* trampoline(void* self) noexcept;

    pthread_t handle_{};
#endif
    Entry entry_{nullptr};
    void* context_{nullptr};
    std::array<char, kMaxNameLength + 1U> name_{};
    bool running_{false};
};

}  // namespace common

#endif  // COMMON_SYSTEM_THREAD_HPP
