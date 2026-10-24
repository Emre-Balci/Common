/// @file float.hpp
/// @brief Floating-point environment control and safe numeric conversions.

#ifndef COMMON_NUMERIC_FLOAT_HPP
#define COMMON_NUMERIC_FLOAT_HPP

#include <cmath>
#include <limits>
#include <type_traits>

#include "common/core/config.hpp"
#include "common/core/status.hpp"

namespace common {

static_assert(std::numeric_limits<float>::is_iec559, "float must be IEEE-754 binary32");
static_assert(std::numeric_limits<double>::is_iec559, "double must be IEEE-754 binary64");

/// Enables flush-to-zero and denormals-are-zero for the calling thread.
/// Denormal operands can make arithmetic 10-100x slower, a source of latency spikes. The setting
/// is per thread and affects every FP operation on it, including library code.
/// @return kNotSupported on architectures without the control.
Status enable_flush_denormals() noexcept;
Status disable_flush_denormals() noexcept;
[[nodiscard]] bool denormals_flushed() noexcept;

/// Sticky IEEE-754 exception flags of the calling thread.
struct FpExceptions final {
    bool invalid{false};
    bool divide_by_zero{false};
    bool overflow{false};
    bool underflow{false};
    bool inexact{false};

    /// Invalid, divide-by-zero and overflow indicate a computation error; underflow and inexact
    /// are routine.
    [[nodiscard]] constexpr bool any_error() const noexcept {
        return invalid || divide_by_zero || overflow;
    }
};

void clear_fp_exceptions() noexcept;
[[nodiscard]] FpExceptions test_fp_exceptions() noexcept;

/// Makes invalid, divide-by-zero and overflow raise SIGFPE on the calling thread.
/// Intended for test and debug builds to locate the first faulty operation.
Status enable_fp_traps() noexcept;
Status disable_fp_traps() noexcept;

/// @return true if the calling thread uses the default round-to-nearest-even mode.
[[nodiscard]] bool rounding_is_nearest() noexcept;

/// Tolerance comparison; never use == on computed floating-point values.
/// @return true if |a - b| <= abs_tol or |a - b| <= rel_tol * max(|a|, |b|). NaN and infinities
///         never compare equal.
template <typename F>
[[nodiscard]] bool approx_equal(const F a, const F b, const F abs_tol, const F rel_tol) noexcept {
    static_assert(std::is_floating_point<F>::value, "F must be a floating-point type");
    bool equal{false};
    if (std::isfinite(a) && std::isfinite(b)) {
        const F diff{std::fabs(a - b)};
        const F scale{std::fmax(std::fabs(a), std::fabs(b))};
        equal = (diff <= abs_tol) || (diff <= (rel_tol * scale));
    }
    return equal;
}

/// Converts with truncation toward zero, saturating at the limits of I. NaN maps to 0.
/// A plain static_cast is undefined behaviour when the value is out of range.
template <typename I, typename F>
[[nodiscard]] I saturating_cast(const F value) noexcept {
    static_assert(std::is_integral<I>::value, "I must be an integral type");
    static_assert(std::is_floating_point<F>::value, "F must be a floating-point type");
    constexpr I kMax{std::numeric_limits<I>::max()};
    constexpr I kMin{std::numeric_limits<I>::min()};
    I result{0};
    if (std::isnan(value)) {
        result = 0;
    } else if (value >= static_cast<F>(kMax)) {
        result = kMax;  // static_cast<F>(kMax) may round up to 2^N, which is out of range.
    } else if (value <= static_cast<F>(kMin)) {
        result = kMin;
    } else {
        result = static_cast<I>(value);
    }
    return result;
}

/// Rounds to nearest (ties to even, the current rounding mode) and converts with saturation.
/// Typical use: converting a price to an integer number of ticks.
template <typename I, typename F>
[[nodiscard]] I saturating_round(const F value) noexcept {
    return saturating_cast<I>(std::nearbyint(value));
}

}  // namespace common

#endif  // COMMON_NUMERIC_FLOAT_HPP
