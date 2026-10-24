#include <cstdint>
#include <limits>

#include "common/control/debouncer.hpp"
#include "common/control/hysteresis.hpp"
#include "common/control/low_pass_filter.hpp"
#include "common/control/pid_controller.hpp"
#include "common/control/slew_rate_limiter.hpp"
#include "common/numeric/float.hpp"
#include "test_framework.hpp"

namespace {

constexpr double kPi{3.14159265358979323846};
constexpr double kNan{std::numeric_limits<double>::quiet_NaN()};

bool near(const double actual, const double expected) noexcept {
    return common::approx_equal(actual, expected, 1e-9, 1e-12);
}

common::PidConfig<double> pid_config(const double kp, const double ki, const double kd) noexcept {
    common::PidConfig<double> config{};
    config.kp = kp;
    config.ki = ki;
    config.kd = kd;
    config.sample_period = 0.1;
    config.output_min = -1.0;
    config.output_max = 1.0;
    return config;
}

}  // namespace

TEST_CASE(low_pass_filter_smooths) {
    common::LowPassFilter<double> filter{0.5};
    CHECK(near(filter.update(10.0), 10.0));  // The first sample initialises the state.
    CHECK(near(filter.update(20.0), 15.0));
    CHECK(near(filter.update(20.0), 17.5));
    CHECK(near(filter.update(kNan), 17.5));  // Ignored.
    for (std::uint32_t i{0U}; i < 100U; ++i) {
        static_cast<void>(filter.update(20.0));
    }
    CHECK(near(filter.output(), 20.0));

    filter.reset();
    CHECK(near(filter.update(-3.0), -3.0));
    filter.reset(1.0);
    CHECK(near(filter.update(3.0), 2.0));

    // fc = 1 / (2 pi) Hz and dt = 1 s give a time constant of 1 s, so alpha = 1 / (1 + 1).
    const common::LowPassFilter<double> tuned{common::LowPassFilter<double>::from_cutoff(1.0 / (2.0 * kPi), 1.0)};
    CHECK(near(tuned.alpha(), 0.5));

    common::LowPassFilter<float> single{0.25F};
    CHECK(common::approx_equal(single.update(4.0F), 4.0F, 1e-6F, 1e-6F));
}

TEST_CASE(slew_rate_limiter_limits_steps) {
    common::SlewRateLimiter<double> slew{1.0, 2.0};
    CHECK(near(slew.update(10.0), 1.0));
    CHECK(near(slew.update(10.0), 2.0));
    CHECK(near(slew.update(1.5), 1.5));  // Within one falling step: lands exactly.
    CHECK(near(slew.update(-10.0), -0.5));
    CHECK(near(slew.update(kNan), -0.5));
    slew.reset(5.0);
    CHECK(near(slew.output(), 5.0));
}

TEST_CASE(pid_proportional_and_limits) {
    common::PidController<double> pid{pid_config(0.5, 0.0, 0.0)};
    CHECK(near(pid.update(1.0, 0.0), 0.5));
    CHECK(near(pid.update(100.0, 0.0), 1.0));
    CHECK(near(pid.update(-100.0, 0.0), -1.0));
}

TEST_CASE(pid_integral_anti_windup) {
    common::PidController<double> pid{pid_config(0.0, 10.0, 0.0)};
    // Each step adds ki * error * dt = 1. Without anti-windup the integrator would reach 100.
    for (std::uint32_t i{0U}; i < 100U; ++i) {
        static_cast<void>(pid.update(1.0, 0.0));
    }
    CHECK(near(pid.output(), 1.0));
    CHECK(near(pid.integral(), 1.0));
    // So one step of opposite error leaves saturation at once.
    CHECK(near(pid.update(0.0, 1.0), 0.0));
}

TEST_CASE(pid_derivative_on_measurement) {
    common::PidConfig<double> config{pid_config(0.0, 0.0, 0.1)};
    config.output_min = -100.0;
    config.output_max = 100.0;
    common::PidController<double> pid{config};
    CHECK(near(pid.update(0.0, 0.0), 0.0));
    CHECK(near(pid.update(50.0, 0.0), 0.0));  // Set-point step: no derivative kick.
    CHECK(near(pid.update(50.0, 1.0), -1.0));  // kd * -(1 - 0) / 0.1
    CHECK(near(pid.update(50.0, kNan), -1.0));  // Skipped: output held.
    // The derivative restarts after the gap. A slope across it, (1 - 3) / 0.1, would give -2.
    CHECK(near(pid.update(50.0, 3.0), 0.0));
    CHECK(near(pid.update(50.0, 4.0), -1.0));
    CHECK(pid.fault_count() == 1U);
}

TEST_CASE(pid_faults_hold_output) {
    common::PidController<double> pid{pid_config(0.5, 0.0, 0.0)};
    CHECK(near(pid.update(1.0, 0.0), 0.5));
    CHECK(near(pid.update(kNan, 0.0), 0.5));
    CHECK(pid.fault_count() == 1U);
    // The error overflows to infinity: the step is skipped instead of producing inf or NaN.
    const double huge{std::numeric_limits<double>::max()};
    CHECK(near(pid.update(huge, -huge), 0.5));
    CHECK(pid.fault_count() == 2U);
    common::clear_fp_exceptions();  // The overflow raised a sticky flag; do not leak it.
}

TEST_CASE(pid_bumpless_reset) {
    common::PidController<double> pid{pid_config(1.0, 1.0, 0.0)};
    pid.reset(0.4);
    CHECK(near(pid.output(), 0.4));
    CHECK(near(pid.update(2.0, 2.0), 0.4));  // Zero error keeps the preloaded output.
    pid.reset(5.0);
    CHECK(near(pid.output(), 1.0));  // Clamped to the limits.
}

TEST_CASE(hysteresis_switches_at_thresholds) {
    common::Hysteresis<double> hysteresis{1.0, 2.0};
    CHECK(!hysteresis.update(1.5));
    CHECK(hysteresis.update(2.0));
    CHECK(hysteresis.update(1.5));
    CHECK(hysteresis.update(kNan));  // NaN keeps the state.
    CHECK(!hysteresis.update(1.0));
    CHECK(!hysteresis.update(kNan));

    common::Hysteresis<std::int32_t> level{10, 20, true};
    CHECK(level.update(15));
    CHECK(!level.update(10));
    CHECK(!level.update(19));
    CHECK(level.update(20));
}

TEST_CASE(debouncer_requires_stable_input) {
    common::Debouncer debouncer{3U, 2U};
    CHECK(!debouncer.update(true));
    CHECK(!debouncer.update(true));
    CHECK(debouncer.update(true));  // Third consecutive sample.

    CHECK(debouncer.update(false));
    CHECK(debouncer.update(true));  // A glitch restarts the count.
    CHECK(debouncer.update(false));
    CHECK(!debouncer.update(false));

    CHECK(!debouncer.update(true));
    CHECK(!debouncer.update(false));
    CHECK(!debouncer.update(true));
    CHECK(!debouncer.update(true));
    CHECK(debouncer.update(true));
    debouncer.reset(false);
    CHECK(!debouncer.state());
}
