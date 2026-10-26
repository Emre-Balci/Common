#include "common/system/memory.hpp"

#if !defined(_WIN32)
#include <sys/mman.h>

#include <cerrno>
#endif

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#if !defined(_WIN32)
#include "detail/posix_status.hpp"
#endif

namespace common {

Status lock_process_memory() noexcept {
#if defined(_WIN32)
    // Windows can only lock given address ranges (VirtualLock), up to the working-set minimum.
    return Status::kNotSupported;
#elif defined(__APPLE__)
    // macOS and iOS declare mlockall() but do not implement it; mlock() locks given ranges.
    return Status::kNotSupported;
#else
    Status status{Status::kOk};
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        status = detail::status_from_errno(errno);
    }
    return status;
#endif
}

Status configure_allocator_for_rt() noexcept {
#if defined(__GLIBC__)
    // mallopt returns 1 on success.
    const bool trim_disabled{::mallopt(M_TRIM_THRESHOLD, -1) == 1};
    const bool mmap_disabled{::mallopt(M_MMAP_MAX, 0) == 1};
    return (trim_disabled && mmap_disabled) ? Status::kOk : Status::kSystemError;
#else
    return Status::kNotSupported;
#endif
}

}  // namespace common
