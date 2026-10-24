/// @file posix_status.hpp
/// @brief Internal: maps POSIX error numbers to Status.

#ifndef COMMON_SRC_POSIX_STATUS_HPP
#define COMMON_SRC_POSIX_STATUS_HPP

#include <cerrno>

#include "common/status.hpp"

namespace common {
namespace detail {

[[nodiscard]] inline Status status_from_errno(const int error) noexcept {
    Status status{Status::kSystemError};
    switch (error) {
        case 0:
            status = Status::kOk;
            break;
        case EPERM:
        case EACCES:
            status = Status::kPermissionDenied;
            break;
        case ENOMEM:
        case EAGAIN:
            status = Status::kResourceLimit;
            break;
        case EINVAL:
        case ESRCH:
        case ERANGE:
            status = Status::kInvalidArgument;
            break;
        case ENOSYS:
        case ENOTSUP:
            status = Status::kNotSupported;
            break;
        case EINTR:
            status = Status::kInterrupted;
            break;
        default:
            status = Status::kSystemError;
            break;
    }
    return status;
}

}  // namespace detail
}  // namespace common

#endif  // COMMON_SRC_POSIX_STATUS_HPP
