/// @file pid_controller.hpp
/// @brief Discrete PID controller with anti-windup and derivative filtering.

#ifndef COMMON_CONTROL_PID_CONTROLLER_HPP
#define COMMON_CONTROL_PID_CONTROLLER_HPP

#include <cmath>
#include <cstdint>
#include <type_traits>

#include "common/core/assert.hpp"
#include "common/core/config.hpp"

namespace common {

namespace detail {

template <typename F>
constexpr F clamp_value(const F value, const F low, const F high) noexcept {
    return (value < low) ? low : ((value > high) ? high : value);
}

}  // namespace detail

/// PID controller parameters. Gains are in output units per error unit (kp), per error unit
/// and second (ki), and per error unit per second (kd).
template <typename F>
struct PidConfig final {
    F kp{0};
    F ki{0};
    F kd{0};
    /// Seconds between update() calls; must be positive.
    F sample_period{0};
    /// Output limits; the integrator is held inside them as well. Must be finite, min < max.
    F output_min{-1};
    F output_max{1};
    /// Low-pass coefficient for the derivative term, in (0, 1]; 1 disables the filter.
    F derivative_alpha{1};
};

/// Discrete PID controller for a fixed sample period.
///
/// - P acts on the error. D acts on the measurement, not the error, so a set-point step causes
///   no derivative kick; the D term is low-pass filtered against measurement noise.
/// - Anti-windup: the integrator does not accumulate while the output is saturated in the
///   direction the accumulation would push, and it is clamped to the output limits, so the
///   controller leaves saturation as soon as the error changes sign.
/// - A non-finite input, or an intermediate result that overflows, skips the step: the output
///   and the integrator are held, fault_count() is incremented, and the derivative restarts
///   from the next valid measurement (no slope is computed across the gap).
template <typename F>
class PidController final {
    static_assert(std::is_floating_point<F>::value, "F must be a floating-point type");

public:
    explicit PidController(const PidConfig<F>& config) noexcept : config_{config} {
        COMMON_ASSERT(std::isfinite(config.kp) && std::isfinite(config.ki) && std::isfinite(config.kd));
        COMMON_ASSERT(std::isfinite(config.sample_period) && (config.sample_period > F{0}));
        COMMON_ASSERT(std::isfinite(config.output_min) && std::isfinite(config.output_max));
        COMMON_ASSERT(config.output_min < config.output_max);
        COMMON_ASSERT((config.derivative_alpha > F{0}) && (config.derivative_alpha <= F{1}));
    }

    /// One control step; call once per sample period. @return the new output.
    F update(const F setpoint, const F measurement) noexcept {
        if (std::isfinite(setpoint) && std::isfinite(measurement)) {
            const F error{setpoint - measurement};
            const F proportional{config_.kp * error};

            const F slope{has_previous_ ? ((previous_measurement_ - measurement) / config_.sample_period) : F{0}};
            const F derivative_state{derivative_state_ + (config_.derivative_alpha * (slope - derivative_state_))};
            const F derivative{config_.kd * derivative_state};

            const F increment{config_.ki * error * config_.sample_period};
            const F unsaturated{proportional + integral_ + increment + derivative};
            const bool winds_up{((unsaturated > config_.output_max) && (increment > F{0})) ||
                                ((unsaturated < config_.output_min) && (increment < F{0}))};
            const F accumulated{winds_up ? integral_ : (integral_ + increment)};
            const F integral{detail::clamp_value(accumulated, config_.output_min, config_.output_max)};
            const F output{proportional + integral + derivative};

            if (std::isfinite(derivative_state) && std::isfinite(output)) {
                derivative_state_ = derivative_state;
                integral_ = integral;
                output_ = detail::clamp_value(output, config_.output_min, config_.output_max);
                previous_measurement_ = measurement;
                has_previous_ = true;
            } else {
                skip_step();
            }
        } else {
            skip_step();
        }
        return output_;
    }

    /// Clears the derivative history and preloads the integrator so that a zero error yields
    /// @p initial_output (clamped to the limits). Use it for a bumpless switch from manual
    /// control to this controller.
    void reset(const F initial_output = F{0}) noexcept {
        const F preload{std::isfinite(initial_output) ? initial_output : F{0}};
        integral_ = detail::clamp_value(preload, config_.output_min, config_.output_max);
        output_ = integral_;
        derivative_state_ = F{0};
        previous_measurement_ = F{0};
        has_previous_ = false;
    }

    [[nodiscard]] F output() const noexcept {
        return output_;
    }

    [[nodiscard]] F integral() const noexcept {
        return integral_;
    }

    /// Number of skipped steps (non-finite input or overflow) since construction.
    [[nodiscard]] std::uint64_t fault_count() const noexcept {
        return fault_count_;
    }

    [[nodiscard]] const PidConfig<F>& config() const noexcept {
        return config_;
    }

private:
    void skip_step() noexcept {
        ++fault_count_;
        has_previous_ = false;  // The previous measurement is now more than one period old.
    }

    PidConfig<F> config_;
    F integral_{0};
    F derivative_state_{0};
    F previous_measurement_{0};
    F output_{0};
    std::uint64_t fault_count_{0U};
    bool has_previous_{false};
};

}  // namespace common

#endif  // COMMON_CONTROL_PID_CONTROLLER_HPP
