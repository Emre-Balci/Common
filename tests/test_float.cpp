#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>

#include "common/float.hpp"
#include "common/status.hpp"
#include "test_framework.hpp"

TEST_CASE(float_approx_equal) {
    CHECK(common::approx_equal(0.1 + 0.2, 0.3, 1e-12, 1e-12));
    CHECK(!common::approx_equal(1.0, 1.001, 1e-9, 1e-9));
    CHECK(common::approx_equal(1e9, 1e9 + 1.0, 0.0, 1e-6));
    const double nan{std::numeric_limits<double>::quiet_NaN()};
    CHECK(!common::approx_equal(nan, nan, 1.0, 1.0));
}

TEST_CASE(float_saturating_conversions) {
    const double inf{std::numeric_limits<double>::infinity()};
    const double nan{std::numeric_limits<double>::quiet_NaN()};
    CHECK(common::saturating_cast<std::int32_t>(1e20) == std::numeric_limits<std::int32_t>::max());
    CHECK(common::saturating_cast<std::int32_t>(-1e20) == std::numeric_limits<std::int32_t>::min());
    CHECK(common::saturating_cast<std::int64_t>(inf) == std::numeric_limits<std::int64_t>::max());
    CHECK(common::saturating_cast<std::int64_t>(nan) == 0);
    CHECK(common::saturating_cast<std::uint32_t>(-5.0) == 0U);
    CHECK(common::saturating_cast<std::int32_t>(-2.9) == -2);
    CHECK(common::saturating_round<std::int64_t>(2.5) == 2);  // Ties to even.
    CHECK(common::saturating_round<std::int64_t>(3.5) == 4);
    CHECK(common::saturating_round<std::int64_t>(-1.6) == -2);
    CHECK(common::rounding_is_nearest());
}

TEST_CASE(float_exception_flags) {
    volatile double zero{0.0};
    volatile double one{1.0};
    common::clear_fp_exceptions();
    CHECK(!common::test_fp_exceptions().any_error());
    const volatile double inf{one / zero};
    static_cast<void>(inf);
    CHECK(common::test_fp_exceptions().divide_by_zero);
    const volatile double nan{zero / zero};
    static_cast<void>(nan);
    CHECK(common::test_fp_exceptions().invalid);
    common::clear_fp_exceptions();
    CHECK(!common::test_fp_exceptions().any_error());
}

TEST_CASE(float_flush_denormals_is_per_thread) {
    // Run on a separate thread: the setting must not leak into other tests.
    bool flushed{false};
    bool result_is_zero{false};
    std::thread worker{[&flushed, &result_is_zero]() {
        if (common::is_ok(common::enable_flush_denormals())) {
            flushed = common::denormals_flushed();
            volatile float tiny{std::numeric_limits<float>::min()};
            volatile float half{0.5F};
            const float product{tiny * half};  // Would be a denormal without FTZ.
            result_is_zero = (std::fpclassify(product) == FP_ZERO);
        }
    }};
    worker.join();
    CHECK(flushed);
    CHECK(result_is_zero);
    CHECK(!common::denormals_flushed());
}
