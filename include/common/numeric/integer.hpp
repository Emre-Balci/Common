/// @file integer.hpp
/// @brief Overflow-checked and saturating integer arithmetic and conversions.
///
/// Signed overflow is undefined behaviour and unsigned wrap-around is rarely what a computation
/// means (CERT INT30-C, INT31-C, INT32-C). Where an operand can be out of range, e.g. a quantity
/// times a price or a sensor value scaled to a bus signal, use these functions instead of the
/// built-in operators:
/// - checked_*  return Result<T>: the exact value, or kOverflow (kInvalidArgument for x / 0).
/// - saturating_* clamp the exact result to the limits of T.
///
/// The checks are written in portable C++ (no compiler built-ins), are constexpr, and take a
/// constant number of steps. Both operands must have the same type, so no implicit conversion
/// hides in a call.

#ifndef COMMON_NUMERIC_INTEGER_HPP
#define COMMON_NUMERIC_INTEGER_HPP

#include <cstdint>
#include <limits>
#include <type_traits>

#include "common/core/config.hpp"
#include "common/core/status.hpp"

namespace common {
namespace detail {

template <typename T>
inline constexpr bool kIsCheckedInteger{std::is_integral<T>::value && (!std::is_same<T, bool>::value)};

// Each predicate evaluates the condition on the operands only, so it never overflows itself.
// Operands narrower than int are promoted; the promoted arithmetic below cannot overflow int.

template <typename T>
constexpr bool add_overflows(const T a, const T b) noexcept {
    bool overflow{false};
    if constexpr (std::is_signed<T>::value) {
        overflow = (b > T{0}) ? (a > (std::numeric_limits<T>::max() - b)) : (a < (std::numeric_limits<T>::min() - b));
    } else {
        overflow = a > (std::numeric_limits<T>::max() - b);
    }
    return overflow;
}

template <typename T>
constexpr bool sub_overflows(const T a, const T b) noexcept {
    bool overflow{false};
    if constexpr (std::is_signed<T>::value) {
        overflow = (b < T{0}) ? (a > (std::numeric_limits<T>::max() + b)) : (a < (std::numeric_limits<T>::min() + b));
    } else {
        overflow = a < b;
    }
    return overflow;
}

template <typename T>
constexpr bool mul_overflows(const T a, const T b) noexcept {
    bool overflow{false};
    if constexpr (std::is_signed<T>::value) {
        constexpr T kMax{std::numeric_limits<T>::max()};
        constexpr T kMin{std::numeric_limits<T>::min()};
        if (a > T{0}) {
            overflow = (b > T{0}) ? (a > (kMax / b)) : (b < (kMin / a));
        } else if (b > T{0}) {
            overflow = a < (kMin / b);
        } else {
            overflow = (a != T{0}) && (b < (kMax / a));
        }
    } else {
        overflow = (b != T{0}) && (a > (std::numeric_limits<T>::max() / b));
    }
    return overflow;
}

template <typename T>
constexpr bool is_negative(const T value) noexcept {
    bool negative{false};
    if constexpr (std::is_signed<T>::value) {
        negative = value < T{0};
    } else {
        static_cast<void>(value);  // An unsigned value is never negative.
    }
    return negative;
}

/// Sign of the exact result of a * b when it overflows.
template <typename T>
constexpr bool product_is_negative(const T a, const T b) noexcept {
    return is_negative(a) != is_negative(b);
}

}  // namespace detail

/// @return a + b, or kOverflow.
template <typename T>
[[nodiscard]] constexpr Result<T> checked_add(const T a, const T b) noexcept {
    static_assert(detail::kIsCheckedInteger<T>, "T must be an integer type other than bool");
    return detail::add_overflows(a, b) ? Result<T>::failure(Status::kOverflow)
                                       : Result<T>::success(static_cast<T>(a + b));
}

/// @return a - b, or kOverflow (for unsigned T: when b > a).
template <typename T>
[[nodiscard]] constexpr Result<T> checked_sub(const T a, const T b) noexcept {
    static_assert(detail::kIsCheckedInteger<T>, "T must be an integer type other than bool");
    return detail::sub_overflows(a, b) ? Result<T>::failure(Status::kOverflow)
                                       : Result<T>::success(static_cast<T>(a - b));
}

/// @return a * b, or kOverflow.
template <typename T>
[[nodiscard]] constexpr Result<T> checked_mul(const T a, const T b) noexcept {
    static_assert(detail::kIsCheckedInteger<T>, "T must be an integer type other than bool");
    return detail::mul_overflows(a, b) ? Result<T>::failure(Status::kOverflow)
                                       : Result<T>::success(static_cast<T>(a * b));
}

/// @return a / b truncated toward zero, kInvalidArgument if b == 0, or kOverflow for min / -1.
template <typename T>
[[nodiscard]] constexpr Result<T> checked_div(const T a, const T b) noexcept {
    static_assert(detail::kIsCheckedInteger<T>, "T must be an integer type other than bool");
    bool overflow{false};
    if constexpr (std::is_signed<T>::value) {
        overflow = (a == std::numeric_limits<T>::min()) && (b == T{-1});
    }
    Result<T> result{Result<T>::failure(Status::kInvalidArgument)};
    if (overflow) {
        result = Result<T>::failure(Status::kOverflow);
    } else if (b != T{0}) {
        result = Result<T>::success(static_cast<T>(a / b));
    } else {
        // Division by zero: keep kInvalidArgument.
    }
    return result;
}

/// @return a + b clamped to [min, max] of T.
template <typename T>
[[nodiscard]] constexpr T saturating_add(const T a, const T b) noexcept {
    static_assert(detail::kIsCheckedInteger<T>, "T must be an integer type other than bool");
    T result{std::numeric_limits<T>::max()};
    if (!detail::add_overflows(a, b)) {
        result = static_cast<T>(a + b);
    } else if (detail::is_negative(b)) {
        result = std::numeric_limits<T>::min();
    } else {
        // Positive overflow: keep max.
    }
    return result;
}

/// @return a - b clamped to [min, max] of T (0 for unsigned T when b > a).
template <typename T>
[[nodiscard]] constexpr T saturating_sub(const T a, const T b) noexcept {
    static_assert(detail::kIsCheckedInteger<T>, "T must be an integer type other than bool");
    T result{std::numeric_limits<T>::min()};
    if (!detail::sub_overflows(a, b)) {
        result = static_cast<T>(a - b);
    } else if (detail::is_negative(b)) {
        result = std::numeric_limits<T>::max();
    } else {
        // Negative overflow (or unsigned underflow): keep min.
    }
    return result;
}

/// @return a * b clamped to [min, max] of T.
template <typename T>
[[nodiscard]] constexpr T saturating_mul(const T a, const T b) noexcept {
    static_assert(detail::kIsCheckedInteger<T>, "T must be an integer type other than bool");
    T result{std::numeric_limits<T>::max()};
    if (!detail::mul_overflows(a, b)) {
        result = static_cast<T>(a * b);
    } else if (detail::product_is_negative(a, b)) {
        result = std::numeric_limits<T>::min();
    } else {
        // Positive overflow: keep max.
    }
    return result;
}

/// @return true if @p value can be represented exactly in To (like C++20 std::in_range).
template <typename To, typename From>
[[nodiscard]] constexpr bool in_range(const From value) noexcept {
    static_assert(detail::kIsCheckedInteger<To>, "To must be an integer type other than bool");
    static_assert(detail::kIsCheckedInteger<From>, "From must be an integer type other than bool");
    bool fits{false};
    if constexpr (std::is_signed<From>::value && std::is_signed<To>::value) {
        const std::intmax_t wide{value};
        fits = (wide >= std::intmax_t{std::numeric_limits<To>::min()}) &&
               (wide <= std::intmax_t{std::numeric_limits<To>::max()});
    } else if constexpr (std::is_signed<From>::value) {
        // Signed to unsigned.
        fits = (value >= From{0}) &&
               (static_cast<std::uintmax_t>(value) <= std::uintmax_t{std::numeric_limits<To>::max()});
    } else if constexpr (std::is_signed<To>::value) {
        // Unsigned to signed.
        const std::uintmax_t wide{value};
        fits = wide <= static_cast<std::uintmax_t>(std::numeric_limits<To>::max());
    } else {
        const std::uintmax_t wide{value};
        fits = wide <= std::uintmax_t{std::numeric_limits<To>::max()};
    }
    return fits;
}

/// Integer conversion that reports a value To cannot represent instead of wrapping it.
/// @return the converted value, or kOverflow.
template <typename To, typename From>
[[nodiscard]] constexpr Result<To> checked_narrow(const From value) noexcept {
    return in_range<To>(value) ? Result<To>::success(static_cast<To>(value)) : Result<To>::failure(Status::kOverflow);
}

/// Integer conversion clamped to the limits of To (the integer counterpart of saturating_cast).
template <typename To, typename From>
[[nodiscard]] constexpr To saturating_narrow(const From value) noexcept {
    To result{std::numeric_limits<To>::max()};
    if (in_range<To>(value)) {
        result = static_cast<To>(value);
    } else if (detail::is_negative(value)) {
        result = std::numeric_limits<To>::min();
    } else {
        // Above the range: keep max.
    }
    return result;
}

}  // namespace common

#endif  // COMMON_NUMERIC_INTEGER_HPP
