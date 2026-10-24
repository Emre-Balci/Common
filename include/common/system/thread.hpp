/// @file thread.hpp
/// @brief Explicit scheduling policy, priority and CPU affinity for real-time threads.

#ifndef COMMON_SYSTEM_THREAD_HPP
#define COMMON_SYSTEM_THREAD_HPP

#include <pthread.h>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

#include "common/core/status.hpp"

namespace common {

enum class SchedPolicy : std::uint8_t {
    kOther,       ///< SCHED_OTHER (CFS); priority must be 0.
    kFifo,        ///< SCHED_FIFO; priority 1..99.
    kRoundRobin,  ///< SCHED_RR; priority 1..99.
};

/// Set of logical CPUs. An empty set means "do not change affinity".
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
    /// CPUs the thread may run on; empty keeps the inherited affinity.
    CpuSet affinity{};
    /// Stack size in bytes; 0 uses the system default.
    std::size_t stack_size{0U};
};

/// Applies a policy and priority to the calling thread.
/// SCHED_FIFO / SCHED_RR need CAP_SYS_NICE or an RLIMIT_RTPRIO grant (see README).
Status set_current_thread_scheduling(SchedPolicy policy, std::int32_t priority) noexcept;
Status get_current_thread_scheduling(SchedPolicy& policy, std::int32_t& priority) noexcept;
Status set_current_thread_affinity(const CpuSet& cpus) noexcept;
Status set_current_thread_name(const char* name) noexcept;

/// Applies name, affinity and scheduling (in that order) to the calling thread, e.g. main().
/// Stops at, and returns, the first error. stack_size is ignored.
Status apply_to_current_thread(const ThreadConfig& config) noexcept;

/// @return the CPU the caller is running on, or -1 on error.
[[nodiscard]] std::int32_t current_cpu() noexcept;

/// pthread wrapper that creates the thread with its scheduling attributes already applied
/// (PTHREAD_EXPLICIT_SCHED), so it never runs with inherited, unintended parameters.
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

    static void* trampoline(void* self) noexcept;

    pthread_t handle_{};
    Entry entry_{nullptr};
    void* context_{nullptr};
    std::array<char, kMaxNameLength + 1U> name_{};
    bool running_{false};
};

}  // namespace common

#endif  // COMMON_SYSTEM_THREAD_HPP
