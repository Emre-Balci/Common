/// @file win32_status.hpp
/// @brief Internal (Windows): maps Windows error codes to Status.

#ifndef COMMON_SRC_DETAIL_WIN32_STATUS_HPP
#define COMMON_SRC_DETAIL_WIN32_STATUS_HPP

#include "common/core/status.hpp"
#include "detail/win32.hpp"

namespace common {
namespace detail {

/// Maps a GetLastError() code to the Status that the corresponding errno value has on Linux (see
/// posix_status.hpp), so that a failure reports the same Status on both platforms.
[[nodiscard]] inline Status status_from_win32(const DWORD error) noexcept {
    Status status{Status::kSystemError};
    switch (error) {
        case ERROR_SUCCESS:
            status = Status::kOk;
            break;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_PRIVILEGE_NOT_HELD:
            status = Status::kPermissionDenied;
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
        case ERROR_TOO_MANY_OPEN_FILES:
        case ERROR_HANDLE_DISK_FULL:
        case ERROR_DISK_FULL:
        case ERROR_DISK_QUOTA_EXCEEDED:
        case ERROR_FILE_TOO_LARGE:
        case ERROR_NO_SYSTEM_RESOURCES:
        case ERROR_COMMITMENT_LIMIT:
        case ERROR_WORKING_SET_QUOTA:
        case ERROR_PAGEFILE_QUOTA:
        case ERROR_NOT_ENOUGH_QUOTA:
            status = Status::kResourceLimit;
            break;
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_HANDLE:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_DIRECTORY:
        case ERROR_NEGATIVE_SEEK:
        case ERROR_CANT_RESOLVE_FILENAME:
            status = Status::kInvalidArgument;
            break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
            status = Status::kNotFound;
            break;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            status = Status::kAlreadyExists;
            break;
        case ERROR_INVALID_FUNCTION:
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED:
            status = Status::kNotSupported;
            break;
        case ERROR_OPERATION_ABORTED:
            status = Status::kInterrupted;
            break;
        case ERROR_TIMEOUT:
        case ERROR_SEM_TIMEOUT:
            status = Status::kTimeout;
            break;
        case ERROR_ARITHMETIC_OVERFLOW:
            status = Status::kOverflow;
            break;
        default:
            status = Status::kSystemError;
            break;
    }
    return status;
}

/// The Status of the calling thread's last Windows error.
[[nodiscard]] inline Status last_win32_error() noexcept {
    return status_from_win32(::GetLastError());
}

}  // namespace detail
}  // namespace common

#endif  // COMMON_SRC_DETAIL_WIN32_STATUS_HPP
