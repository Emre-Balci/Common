/// @file status.hpp
/// @brief Exception-free error reporting: Status codes and Result<T>.

#ifndef COMMON_CORE_STATUS_HPP
#define COMMON_CORE_STATUS_HPP

#include <cstdint>
#include <type_traits>

#include "common/core/config.hpp"

namespace common {

/// Error code returned by every fallible operation in this library.
/// [[nodiscard]] on the enumeration forces callers to inspect or explicitly discard each result.
enum class [[nodiscard]] Status : std::uint8_t {
    kOk = 0U,
    kInvalidArgument,
    kPermissionDenied,
    kResourceLimit,
    kNotSupported,
    kSystemError,
    kInterrupted,
    kDeadlineMissed,
    kAlreadyRunning,
    kNotRunning,
    kOverflow,
    kNotFound,
    kAlreadyExists,
    kOutOfRange,
    kTimeout,
    kDisconnected,
};

[[nodiscard]] constexpr bool is_ok(const Status status) noexcept {
    return status == Status::kOk;
}

[[nodiscard]] constexpr const char* to_string(const Status status) noexcept {
    const char* text{"unknown"};
    switch (status) {
        case Status::kOk:
            text = "ok";
            break;
        case Status::kInvalidArgument:
            text = "invalid argument";
            break;
        case Status::kPermissionDenied:
            text = "permission denied";
            break;
        case Status::kResourceLimit:
            text = "resource limit exceeded";
            break;
        case Status::kNotSupported:
            text = "not supported";
            break;
        case Status::kSystemError:
            text = "system error";
            break;
        case Status::kInterrupted:
            text = "interrupted";
            break;
        case Status::kDeadlineMissed:
            text = "deadline missed";
            break;
        case Status::kAlreadyRunning:
            text = "already running";
            break;
        case Status::kNotRunning:
            text = "not running";
            break;
        case Status::kOverflow:
            text = "arithmetic overflow";
            break;
        case Status::kNotFound:
            text = "not found";
            break;
        case Status::kAlreadyExists:
            text = "already exists";
            break;
        case Status::kOutOfRange:
            text = "out of range";
            break;
        case Status::kTimeout:
            text = "timed out";
            break;
        case Status::kDisconnected:
            text = "disconnected";
            break;
    }
    return text;
}

/// Value-or-error holder without heap allocation or exceptions.
/// T must be nothrow default-constructible so that the error state holds a well-defined value.
template <typename T>
class [[nodiscard]] Result final {
    static_assert(std::is_nothrow_default_constructible<T>::value, "T must be nothrow default-constructible");
    static_assert(std::is_nothrow_copy_constructible<T>::value, "T must be nothrow copy-constructible");

public:
    [[nodiscard]] static constexpr Result success(const T& value) noexcept {
        return Result{value, Status::kOk};
    }

    /// @pre status != Status::kOk
    [[nodiscard]] static constexpr Result failure(const Status status) noexcept {
        return Result{T{}, status};
    }

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return status_ == Status::kOk;
    }

    [[nodiscard]] constexpr Status status() const noexcept {
        return status_;
    }

    /// @pre has_value()
    [[nodiscard]] constexpr const T& value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr T value_or(const T& fallback) const noexcept {
        return has_value() ? value_ : fallback;
    }

private:
    constexpr Result(const T& value, const Status status) noexcept : value_{value}, status_{status} {}

    T value_;
    Status status_;
};

}  // namespace common

#endif  // COMMON_CORE_STATUS_HPP
