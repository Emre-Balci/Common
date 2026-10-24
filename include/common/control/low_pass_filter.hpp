/// @file low_pass_filter.hpp
/// @brief First-order low-pass filter for cyclic real-time loops.

#ifndef COMMON_CONTROL_LOW_PASS_FILTER_HPP
#define COMMON_CONTROL_LOW_PASS_FILTER_HPP

#include <cmath>
#include <type_traits>

#include "common/core/assert.hpp"
#include "common/core/config.hpp"

namespace common {

/// First-order IIR low-pass filter (exponential smoothing): y += alpha * (x - y).
///
/// O(1) per sample, allocation-free, owned by the loop's thread. A non-finite input (NaN,
/// infinity) is skipped and the previous output is held, so one corrupt reading cannot poison
/// the state.
template <typename F>
class LowPassFilter final {
    static_assert(std::is_floating_point<F>::value, "F must be a floating-point type");

public:
    /// @pre 0 < alpha <= 1 (1 passes the input through unchanged).
    explicit LowPassFilter(const F alpha) noexcept : alpha_{alpha} {
        COMMON_ASSERT((alpha > F{0}) && (alpha <= F{1}));
    }

    /// Filter with a -3 dB cut-off at @p cutoff_hz for samples taken every @p sample_period_s
    /// seconds: alpha = dt / (dt + 1 / (2 pi fc)).
    /// @pre cutoff_hz > 0, sample_period_s > 0, both finite
    [[nodiscard]] static LowPassFilter from_cutoff(const F cutoff_hz, const F sample_period_s) noexcept {
        COMMON_ASSERT(std::isfinite(cutoff_hz) && (cutoff_hz > F{0}));
        COMMON_ASSERT(std::isfinite(sample_period_s) && (sample_period_s > F{0}));
        const F time_constant{F{1} / (kTwoPi * cutoff_hz)};
        return LowPassFilter{sample_period_s / (sample_period_s + time_constant)};
    }

    /// Feeds one sample. The first finite sample initialises the state, so the output does not
    /// ramp up from zero. @return the filtered value.
    F update(const F input) noexcept {
        if (std::isfinite(input)) {
            if (initialized_) {
                output_ += alpha_ * (input - output_);
            } else {
                output_ = input;
                initialized_ = true;
            }
        }
        return output_;
    }

    /// Sets the state to @p value, e.g. a known steady state.
    void reset(const F value) noexcept {
        output_ = value;
        initialized_ = true;
    }

    /// Forgets the state: the next sample initialises it again.
    void reset() noexcept {
        output_ = F{0};
        initialized_ = false;
    }

    [[nodiscard]] F output() const noexcept {
        return output_;
    }

    [[nodiscard]] F alpha() const noexcept {
        return alpha_;
    }

private:
    static constexpr F kTwoPi{static_cast<F>(6.283185307179586476925286766559L)};

    F alpha_;
    F output_{0};
    bool initialized_{false};
};

}  // namespace common

#endif  // COMMON_CONTROL_LOW_PASS_FILTER_HPP
