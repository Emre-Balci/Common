/// @file posix_status.hpp
/// @brief Internal: maps POSIX error numbers to Status.

#ifndef COMMON_SRC_DETAIL_POSIX_STATUS_HPP
#define COMMON_SRC_DETAIL_POSIX_STATUS_HPP

#include <cerrno>

#include "common/core/status.hpp"

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
        case EROFS:
            status = Status::kPermissionDenied;
            break;
        case ENOMEM:
        case EAGAIN:
        case ENOSPC:
        case EDQUOT:
        case EMFILE:
        case ENFILE:
        case EFBIG:
            status = Status::kResourceLimit;
            break;
        case EINVAL:
        case ESRCH:
        case ERANGE:
        case EBADF:
        case EISDIR:
        case ENOTDIR:
        case ENAMETOOLONG:
        case ELOOP:
            status = Status::kInvalidArgument;
            break;
        case ENOENT:
            status = Status::kNotFound;
            break;
        case EEXIST:
            status = Status::kAlreadyExists;
            break;
        case ENOSYS:
        case ENOTSUP:  // Same value as EOPNOTSUPP on Linux.
            status = Status::kNotSupported;
            break;
        case EINTR:
            status = Status::kInterrupted;
            break;
        case ETIMEDOUT:
            status = Status::kTimeout;
            break;
        case EOVERFLOW:
            status = Status::kOverflow;
            break;
        default:
            status = Status::kSystemError;
            break;
    }
    return status;
}

}  // namespace detail
}  // namespace common

#endif  // COMMON_SRC_DETAIL_POSIX_STATUS_HPP
