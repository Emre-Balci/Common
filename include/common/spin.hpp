/// @file spin.hpp
/// @brief CPU hint for busy-wait loops.

#ifndef COMMON_SPIN_HPP
#define COMMON_SPIN_HPP

#include <atomic>

#include "common/config.hpp"

namespace common {

/// Tells the CPU the caller is spinning: reduces power and pipeline flushes, and yields the core
/// to a sibling hyper-thread. It does not yield to the OS scheduler.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    // Deviation (MISRA C++:2023 Rule 10.4.1): no portable intrinsic exists for the AArch64 yield hint.
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

}  // namespace common

#endif  // COMMON_SPIN_HPP
