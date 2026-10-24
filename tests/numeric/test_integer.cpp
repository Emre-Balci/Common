#include <cstdint>
#include <limits>

#include "common/core/status.hpp"
#include "common/numeric/integer.hpp"
#include "test_framework.hpp"

namespace {

constexpr std::int32_t kMax{std::numeric_limits<std::int32_t>::max()};
constexpr std::int32_t kMin{std::numeric_limits<std::int32_t>::min()};
constexpr std::uint64_t kU64Max{std::numeric_limits<std::uint64_t>::max()};

template <typename T>
bool overflows(const common::Result<T>& result) noexcept {
    return result.status() == common::Status::kOverflow;
}

// The functions are constexpr: these are checked at compile time.
static_assert(common::checked_add(40, 2).value() == 42, "checked_add is constexpr");
static_assert(common::saturating_mul(kMax, 2) == kMax, "saturating_mul is constexpr");
static_assert(common::in_range<std::uint8_t>(255), "in_range is constexpr");

}  // namespace

TEST_CASE(integer_checked_signed) {
    CHECK(common::checked_add(2, 3).value() == 5);
    CHECK(common::checked_add(kMax, kMin).value() == -1);
    CHECK(overflows(common::checked_add(kMax, 1)));
    CHECK(overflows(common::checked_add(kMin, -1)));

    CHECK(common::checked_sub(-1, kMin).value() == kMax);
    CHECK(overflows(common::checked_sub(kMin, 1)));
    CHECK(overflows(common::checked_sub(0, kMin)));  // -min is not representable.

    CHECK(common::checked_mul(46'340, 46'340).value() == 2'147'395'600);
    CHECK(overflows(common::checked_mul(46'341, 46'341)));
    CHECK(common::checked_mul(kMin / 2, 2).value() == kMin);
    CHECK(overflows(common::checked_mul(kMin / 2, -2)));
    CHECK(overflows(common::checked_mul(kMin, -1)));
    CHECK(common::checked_mul(-1, kMax).value() == -kMax);
    CHECK(common::checked_mul(0, kMin).value() == 0);

    CHECK(common::checked_div(-7, 2).value() == -3);
    CHECK(overflows(common::checked_div(kMin, -1)));
    CHECK(common::checked_div(7, 0).status() == common::Status::kInvalidArgument);

    // Types narrower than int are promoted inside the checks; the limits are still those of T.
    const std::int8_t i8_max{127};
    const std::int8_t i8_min{-128};
    CHECK(common::checked_add(std::int8_t{100}, std::int8_t{27}).value() == i8_max);
    CHECK(overflows(common::checked_add(std::int8_t{100}, std::int8_t{28})));
    CHECK(overflows(common::checked_mul(i8_min, std::int8_t{-1})));
    CHECK(overflows(common::checked_sub(i8_min, std::int8_t{1})));
}

TEST_CASE(integer_checked_unsigned) {
    const std::uint8_t u8_max{255U};
    CHECK(common::checked_add(std::uint8_t{200U}, std::uint8_t{55U}).value() == u8_max);
    CHECK(overflows(common::checked_add(std::uint8_t{200U}, std::uint8_t{56U})));
    CHECK(overflows(common::checked_sub(std::uint32_t{1U}, std::uint32_t{2U})));
    CHECK(common::checked_sub(std::uint32_t{2U}, std::uint32_t{2U}).value() == 0U);

    // uint16 * uint16 is computed in int; 65535 * 65535 would overflow int, so it must be rejected
    // before the multiplication happens.
    const std::uint16_t u16_max{65'535U};
    CHECK(common::checked_mul(std::uint16_t{255U}, std::uint16_t{257U}).value() == u16_max);
    CHECK(overflows(common::checked_mul(std::uint16_t{256U}, std::uint16_t{256U})));
    CHECK(overflows(common::checked_mul(u16_max, u16_max)));
    CHECK(overflows(common::checked_mul(kU64Max, std::uint64_t{2U})));
    CHECK(common::checked_mul(kU64Max, std::uint64_t{0U}).value() == 0U);
    CHECK(common::checked_div(std::uint32_t{7U}, std::uint32_t{0U}).status() == common::Status::kInvalidArgument);
}

TEST_CASE(integer_saturating) {
    CHECK(common::saturating_add(kMax, 1) == kMax);
    CHECK(common::saturating_add(kMin, -5) == kMin);
    CHECK(common::saturating_add(-5, 3) == -2);
    CHECK(common::saturating_sub(kMin, 1) == kMin);
    CHECK(common::saturating_sub(kMax, -1) == kMax);
    CHECK(common::saturating_mul(kMax, -2) == kMin);
    CHECK(common::saturating_mul(kMin, -1) == kMax);
    CHECK(common::saturating_mul(kMin, kMin) == kMax);
    CHECK(common::saturating_mul(-3, -4) == 12);

    const std::uint8_t u8_max{255U};
    CHECK(common::saturating_add(std::uint8_t{250U}, std::uint8_t{10U}) == u8_max);
    CHECK(common::saturating_sub(std::uint32_t{3U}, std::uint32_t{5U}) == 0U);
    CHECK(common::saturating_mul(kU64Max, std::uint64_t{3U}) == kU64Max);
}

TEST_CASE(integer_range_conversions) {
    CHECK(common::in_range<std::int8_t>(127));
    CHECK(!common::in_range<std::int8_t>(128));
    CHECK(common::in_range<std::int8_t>(-128));
    CHECK(!common::in_range<std::int8_t>(-129));
    CHECK(!common::in_range<std::uint32_t>(-1));
    CHECK(common::in_range<std::uint32_t>(std::int64_t{4'294'967'295}));
    CHECK(!common::in_range<std::uint32_t>(std::int64_t{4'294'967'296}));
    CHECK(!common::in_range<std::int32_t>(std::uint32_t{2'147'483'648U}));
    CHECK(common::in_range<std::int64_t>(std::numeric_limits<std::uint32_t>::max()));
    CHECK(!common::in_range<std::int64_t>(kU64Max));
    CHECK(common::in_range<std::uint64_t>(std::uint8_t{0U}));

    const std::int16_t i16_max{32'767};
    const std::int16_t i16_min{-32'768};
    const std::uint16_t u16_value{1234U};
    CHECK(common::saturating_narrow<std::int16_t>(100'000) == i16_max);
    CHECK(common::saturating_narrow<std::int16_t>(-100'000) == i16_min);
    CHECK(common::saturating_narrow<std::uint8_t>(-5) == std::uint8_t{0U});
    CHECK(common::saturating_narrow<std::int32_t>(kU64Max) == kMax);
    CHECK(common::saturating_narrow<std::uint16_t>(std::int32_t{1234}) == u16_value);

    const std::uint8_t u8_max{255U};
    CHECK(common::checked_narrow<std::uint8_t>(255).value() == u8_max);
    CHECK(overflows(common::checked_narrow<std::uint8_t>(256)));
    CHECK(overflows(common::checked_narrow<std::uint64_t>(-1)));
}
