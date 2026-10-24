/// @file slew_rate_limiter.hpp
/// @brief Rate-of-change limiter for set-points and actuator commands.

#ifndef COMMON_CONTROL_SLEW_RATE_LIMITER_HPP
#define COMMON_CONTROL_SLEW_RATE_LIMITER_HPP

#include <cmath>
#include <type_traits>

#include "common/core/assert.hpp"
#include "common/core/config.hpp"

namespace common {

/// Limits how fast a signal may change, e.g. an actuator command or a torque request.
///
/// O(1) per sample, allocation-free, owned by the loop's thread. A non-finite target (NaN,
/// infinity) is skipped and the previous output is held.
template <typename F>
class SlewRateLimiter final {
    static_assert(std::is_floating_point<F>::value, "F must be a floating-point type");

public:
    /// The output may rise by at most @p max_rise and fall by at most @p max_fall per update().
    /// For a rate in units per second, pass rate * sample period.
    /// @pre max_rise >= 0, max_fall >= 0, initial finite, all finite
    SlewRateLimiter(const F max_rise, const F max_fall, const F initial = F{0}) noexcept
        : max_rise_{max_rise}, max_fall_{max_fall}, output_{initial} {
        COMMON_ASSERT(std::isfinite(max_rise) && (max_rise >= F{0}));
        COMMON_ASSERT(std::isfinite(max_fall) && (max_fall >= F{0}));
        COMMON_ASSERT(std::isfinite(initial));
    }

    /// Moves the output toward @p target by at most one step. @return the new output.
    F update(const F target) noexcept {
        if (std::isfinite(target)) {
            const F delta{target - output_};
            if (delta > max_rise_) {
                output_ += max_rise_;
            } else if (delta < (-max_fall_)) {
                output_ -= max_fall_;
            } else {
                output_ = target;  // Reachable in this step: land exactly on it.
            }
        }
        return output_;
    }

    /// @pre value is finite
    void reset(const F value) noexcept {
        COMMON_ASSERT(std::isfinite(value));
        output_ = value;
    }

    [[nodiscard]] F output() const noexcept {
        return output_;
    }

private:
    F max_rise_;
    F max_fall_;
    F output_;
};

}  // namespace common

#endif  // COMMON_CONTROL_SLEW_RATE_LIMITER_HPP
