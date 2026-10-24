#include "common/system/memory.hpp"

#include <sys/mman.h>

#include <cerrno>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "detail/posix_status.hpp"

namespace common {

Status lock_process_memory() noexcept {
    Status status{Status::kOk};
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        status = detail::status_from_errno(errno);
    }
    return status;
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
