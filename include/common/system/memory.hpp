/// @file memory.hpp
/// @brief Process memory set-up that removes page faults from the real-time phase.

#ifndef COMMON_SYSTEM_MEMORY_HPP
#define COMMON_SYSTEM_MEMORY_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/core/config.hpp"
#include "common/core/status.hpp"

namespace common {

/// Locks all current and future pages in RAM (mlockall(MCL_CURRENT | MCL_FUTURE)).
/// @return kResourceLimit if RLIMIT_MEMLOCK is too small, kPermissionDenied without privilege;
///         kNotSupported on Windows, macOS and iOS, which have no equivalent (MappedFile::lock()
///         locks a mapping).
Status lock_process_memory() noexcept;

/// Stops glibc malloc from returning memory to the OS (trim) and from serving requests with
/// mmap, so memory allocated during initialisation stays mapped and locked.
/// @return kNotSupported on non-glibc systems, including Windows.
Status configure_allocator_for_rt() noexcept;

/// Touches @p Bytes of the calling thread's stack so later use does not page-fault.
/// Call once at the start of each real-time thread, after lock_process_memory().
template <std::size_t Bytes>
void prefault_stack() noexcept {
    static_assert(Bytes > 0U, "Bytes must be positive");
    std::array<std::uint8_t, Bytes> buffer;
    volatile std::uint8_t* const bytes{buffer.data()};
    for (std::size_t offset{0U}; offset < Bytes; offset += kPageSize) {
        bytes[offset] = 0U;
    }
}

}  // namespace common

#endif  // COMMON_SYSTEM_MEMORY_HPP
