#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/numeric/decimal.hpp"
#include "test_framework.hpp"

namespace {

using Price = common::Decimal<4U>;
using Quantity = common::Decimal<8U>;
using Cents = common::Decimal<2U>;
using Whole = common::Decimal<0U>;

constexpr std::int64_t kMin{std::numeric_limits<std::int64_t>::min()};
constexpr std::int64_t kMax{std::numeric_limits<std::int64_t>::max()};

common::Span<const char> span_of(const std::string_view text) {
    return common::Span<const char>{text.data(), text.size()};
}

/// parse() as units, or a sentinel for an error; kMin + 1 for kInvalidArgument, kMin + 2 for kOverflow.
template <std::uint32_t N>
std::int64_t parsed(const std::string_view text) {
    const common::Result<common::Decimal<N>> value{common::Decimal<N>::parse(span_of(text))};
    std::int64_t units{kMin + 3};
    if (value.has_value()) {
        units = value.value().units();
    } else if (value.status() == common::Status::kInvalidArgument) {
        units = kMin + 1;
    } else if (value.status() == common::Status::kOverflow) {
        units = kMin + 2;
    } else {
        // Unexpected status: keep the third sentinel.
    }
    return units;
}

template <std::uint32_t N>
std::string formatted(const common::Decimal<N> value) {
    std::array<char, 32U> buffer{};
    const common::Result<std::size_t> length{value.format(common::Span<char>{buffer})};
    return length.has_value() ? std::string{buffer.data(), length.value()} : std::string{"<error>"};
}

constexpr std::int64_t kInvalid{kMin + 1};
constexpr std::int64_t kOverflowed{kMin + 2};

}  // namespace

TEST_CASE(decimal_parse) {
    CHECK(parsed<4U>("42") == 420'000);
    CHECK(parsed<4U>("-0.5") == -5'000);
    CHECK(parsed<4U>(".5") == 5'000);
    CHECK(parsed<4U>("42.") == 420'000);
    CHECK(parsed<4U>("0042.50") == 425'000);
    CHECK(parsed<4U>("+1.25") == 12'500);
    CHECK(parsed<4U>("-0") == 0);
    CHECK(parsed<4U>("1.23450") == 12'345);  // Zeros beyond the precision are exact.
    CHECK(parsed<0U>("7") == 7);

    CHECK(parsed<4U>("") == kInvalid);
    CHECK(parsed<4U>("-") == kInvalid);
    CHECK(parsed<4U>(".") == kInvalid);
    CHECK(parsed<4U>("1.2.3") == kInvalid);
    CHECK(parsed<4U>("1e5") == kInvalid);
    CHECK(parsed<4U>(" 1") == kInvalid);
    CHECK(parsed<4U>("1,000") == kInvalid);
    CHECK(parsed<4U>("--1") == kInvalid);
    CHECK(parsed<4U>("1.23456") == kInvalid);  // Never rounds.
    CHECK(parsed<0U>("1.5") == kInvalid);

    // The limits of the units.
    CHECK(parsed<4U>("922337203685477.5807") == kMax);
    CHECK(parsed<4U>("922337203685477.5808") == kOverflowed);
    CHECK(parsed<4U>("-922337203685477.5808") == kMin);
    CHECK(parsed<4U>("-922337203685477.5809") == kOverflowed);
    CHECK(parsed<0U>("9223372036854775807") == kMax);
    CHECK(parsed<0U>("99999999999999999999999") == kOverflowed);
    CHECK(parsed<18U>("9.223372036854775807") == kMax);
    CHECK(parsed<18U>("0.000000000000000001") == 1);

    static_assert(Cents::parse(common::Span<const char>{"1.25", 4U}).value().units() == 125, "constexpr");
}

TEST_CASE(decimal_format) {
    CHECK(formatted(Price::from_units(-123'400)) == "-12.3400");
    CHECK(formatted(Price::from_units(0)) == "0.0000");
    CHECK(formatted(Price::from_units(-5'000)) == "-0.5000");
    CHECK(formatted(Price::from_units(kMin)) == "-922337203685477.5808");
    CHECK(formatted(Price::from_units(kMax)) == "922337203685477.5807");
    CHECK(formatted(Whole::from_units(7)) == "7");
    CHECK(formatted(Whole::from_units(kMin)) == "-9223372036854775808");
    CHECK(formatted(common::Decimal<18U>::from_units(1)) == "0.000000000000000001");
    static_assert(Price::kMaxFormattedSize == 21U, "sign, 15 digits, point and 4 decimals");
    static_assert(Whole::kMaxFormattedSize == 20U, "sign and 19 digits");

    // Too small a buffer: nothing is written.
    std::array<char, 7U> small{};
    small.fill('#');
    CHECK(Price::from_units(-123'400).format(common::Span<char>{small}).status() == common::Status::kOutOfRange);
    CHECK(small[0U] == '#');
    std::array<char, 8U> exact{};
    CHECK(Price::from_units(-123'400).format(common::Span<char>{exact}).value_or(0U) == 8U);

    // Every formatted value parses back to itself.
    std::uint64_t state{7U};
    for (std::uint32_t i{0U}; i < 10'000U; ++i) {
        state = (state * 6364136223846793005U) + 1442695040888963407U;
        const std::int64_t units{static_cast<std::int64_t>(state)};
        CHECK(parsed<4U>(formatted(Price::from_units(units))) == units);
        CHECK(parsed<9U>(formatted(common::Decimal<9U>::from_units(units))) == units);
    }
}

TEST_CASE(decimal_exact_arithmetic) {
    const Price a{Price::parse(span_of("101.25")).value()};
    const Price b{Price::parse(span_of("0.75")).value()};
    CHECK(a.add(b).value().units() == 1'020'000);
    CHECK(a.subtract(b).value().units() == 1'005'000);
    CHECK(b.subtract(a).value().units() == -1'005'000);
    CHECK(a.negate().value().units() == -1'012'500);
    CHECK(a.multiply(std::int64_t{3}).value().units() == 3'037'500);

    CHECK(Price::from_units(kMax).add(Price::from_units(1)).status() == common::Status::kOverflow);
    CHECK(Price::from_units(kMin).subtract(Price::from_units(1)).status() == common::Status::kOverflow);
    CHECK(Price::from_units(kMin).negate().status() == common::Status::kOverflow);
    CHECK(Price::from_units(kMax).multiply(std::int64_t{2}).status() == common::Status::kOverflow);
    CHECK(Price::from_integer(7).value().units() == 70'000);
    CHECK(Price::from_integer(kMax).status() == common::Status::kOverflow);
    CHECK(Price::scale() == 10'000);

    CHECK(a > b);
    CHECK(b < a);
    CHECK(a >= a);
    CHECK(a <= a);
    CHECK(a == Price::from_units(1'012'500));
    CHECK(a != b);
    CHECK(std::fabs(a.to_double() - 101.25) < 1e-12);
}

TEST_CASE(decimal_rounding_modes) {
    // 101.25 * 0.5 = 50.625 exactly, rounded once to the price's scale and then to cents.
    const Price price{Price::parse(span_of("101.25")).value()};
    const Quantity half{Quantity::parse(span_of("0.5")).value()};
    const Price notional{price.multiply(half, common::RoundingMode::kHalfEven).value()};
    CHECK(notional.units() == 506'250);

    struct Case final {
        common::RoundingMode mode;
        std::int64_t positive;
        std::int64_t negative;
    };
    const std::array<Case, 5U> cases{{{common::RoundingMode::kHalfEven, 5'062, -5'062},
                                      {common::RoundingMode::kHalfAwayFromZero, 5'063, -5'063},
                                      {common::RoundingMode::kTowardZero, 5'062, -5'062},
                                      {common::RoundingMode::kFloor, 5'062, -5'063},
                                      {common::RoundingMode::kCeiling, 5'063, -5'062}}};
    for (const Case& c : cases) {
        CHECK(notional.rescale<2U>(c.mode).value().units() == c.positive);
        CHECK(notional.negate().value().rescale<2U>(c.mode).value().units() == c.negative);
        // The same through a product: -101.25 * 0.5 rounded straight to cents.
        CHECK(Cents::parse(span_of("-101.25")).value().multiply(half, c.mode).value().units() == c.negative);
    }
    // Ties to even look at the kept digit: 1.255 -> 1.26, 1.245 -> 1.24.
    CHECK(Price::from_units(12'550).rescale<2U>(common::RoundingMode::kHalfEven).value().units() == 126);
    CHECK(Price::from_units(12'450).rescale<2U>(common::RoundingMode::kHalfEven).value().units() == 124);
    CHECK(Price::from_units(12'451).rescale<2U>(common::RoundingMode::kHalfEven).value().units() == 125);

    // Division.
    const Price ten{Price::from_integer(10).value()};
    CHECK(ten.divide(3, common::RoundingMode::kHalfEven).value().units() == 33'333);
    CHECK(ten.divide(3, common::RoundingMode::kCeiling).value().units() == 33'334);
    CHECK(ten.divide(-3, common::RoundingMode::kFloor).value().units() == -33'334);
    CHECK(ten.divide(-3, common::RoundingMode::kTowardZero).value().units() == -33'333);
    CHECK(ten.divide(0, common::RoundingMode::kHalfEven).status() == common::Status::kInvalidArgument);
    CHECK(Price::from_units(kMin).divide(-1, common::RoundingMode::kHalfEven).status() ==
          common::Status::kOverflow);
    CHECK(Price::from_units(kMin).divide(1, common::RoundingMode::kHalfEven).value().units() == kMin);

    // Rounding to a tick size.
    const Price raw{Price::parse(span_of("101.237")).value()};
    const Price tick{Price::parse(span_of("0.005")).value()};
    CHECK(raw.round_to_multiple(tick, common::RoundingMode::kFloor).value().units() == 1'012'350);
    CHECK(raw.round_to_multiple(tick, common::RoundingMode::kCeiling).value().units() == 1'012'400);
    CHECK(raw.round_to_multiple(tick, common::RoundingMode::kHalfEven).value().units() == 1'012'350);
    CHECK(raw.negate().value().round_to_multiple(tick, common::RoundingMode::kFloor).value().units() == -1'012'400);
    CHECK(raw.round_to_multiple(Price{}, common::RoundingMode::kFloor).status() == common::Status::kInvalidArgument);
    CHECK(raw.round_to_multiple(tick.negate().value(), common::RoundingMode::kFloor).status() ==
          common::Status::kInvalidArgument);
    CHECK(Price::from_units(kMax).round_to_multiple(Price::from_units(2), common::RoundingMode::kCeiling).status() ==
          common::Status::kOverflow);
}

TEST_CASE(decimal_wide_products_and_rescale) {
    // Products that need the full 128 bits before rounding.
    const Quantity big{Quantity::from_units(kMax)};
    // 1e-8 * (kMax * 1e-8) = kMax * 1e-16, i.e. 92233720368.54775807 units of 1e-8.
    CHECK(Quantity::from_units(1).multiply(big, common::RoundingMode::kHalfEven).value().units() == 92'233'720'369);
    CHECK(Price::from_integer(2).value().multiply(Price::from_units(kMax / 4), common::RoundingMode::kTowardZero)
              .value()
              .units() == ((kMax / 4) * 2));
    CHECK(big.multiply(big, common::RoundingMode::kHalfEven).status() == common::Status::kOverflow);
    CHECK(Price::from_units(kMin).multiply(Price::from_integer(1).value(), common::RoundingMode::kHalfEven)
              .value()
              .units() == kMin);

    // More digits is exact and can overflow; fewer digits rounds and cannot.
    CHECK(Cents::from_units(125).rescale<4U>(common::RoundingMode::kFloor).value().units() == 12'500);
    CHECK(Whole::from_units(kMax).rescale<1U>(common::RoundingMode::kFloor).status() == common::Status::kOverflow);
    CHECK(Price::from_units(kMin).rescale<0U>(common::RoundingMode::kFloor).value().units() ==
          (kMin / 10'000) - 1);
    CHECK(Price::from_units(12'345).rescale<4U>(common::RoundingMode::kFloor).value().units() == 12'345);
}

TEST_CASE(decimal_from_double) {
    CHECK(Price::from_double(0.1, common::RoundingMode::kHalfEven).value().units() == 1'000);
    // 1.005 is stored as 1.00499999999999989...: rounding the binary value gives 1.00.
    CHECK(Cents::from_double(1.005, common::RoundingMode::kHalfAwayFromZero).value().units() == 100);
    CHECK(Whole::from_double(-2.5, common::RoundingMode::kHalfEven).value().units() == -2);
    CHECK(Whole::from_double(-2.5, common::RoundingMode::kHalfAwayFromZero).value().units() == -3);
    CHECK(Whole::from_double(2.5, common::RoundingMode::kFloor).value().units() == 2);
    CHECK(Whole::from_double(-0.0, common::RoundingMode::kHalfEven).value().units() == 0);
    CHECK(Price::from_double(1e-300, common::RoundingMode::kCeiling).value().units() == 1);
    CHECK(Price::from_double(5e-324, common::RoundingMode::kHalfEven).value().units() == 0);
    CHECK(Whole::from_double(9007199254740993.0, common::RoundingMode::kHalfEven).value().units() ==
          9007199254740992);  // The double nearest to 2^53 + 1.
    CHECK(Price::from_double(1e300, common::RoundingMode::kHalfEven).status() == common::Status::kOverflow);
    CHECK(Whole::from_double(9.3e18, common::RoundingMode::kHalfEven).status() == common::Status::kOverflow);
    CHECK(Whole::from_double(-9223372036854775808.0, common::RoundingMode::kHalfEven).value().units() == kMin);
    CHECK(Price::from_double(std::numeric_limits<double>::quiet_NaN(), common::RoundingMode::kHalfEven).status() ==
          common::Status::kInvalidArgument);
    CHECK(Price::from_double(-std::numeric_limits<double>::infinity(), common::RoundingMode::kHalfEven).status() ==
          common::Status::kInvalidArgument);
}
