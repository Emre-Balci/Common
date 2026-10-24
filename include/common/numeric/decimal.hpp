/// @file decimal.hpp
/// @brief Exact decimal fixed-point numbers: checked arithmetic, explicit rounding, and parsing and
///        formatting of text without allocation.
///
/// Prices, quantities and notional values must be exact. A double cannot hold most decimal
/// fractions (0.1 is stored as 0.1000000000000000055...), so sums drift and a computed price may not
/// equal the one in the order book. Decimal<N> stores the value times 10^N in a std::int64_t:
/// - add, subtract and multiplication by an integer are exact, and report kOverflow instead of
///   wrapping;
/// - an operation that drops digits (a product of two scaled values, a quotient, rounding to a tick
///   or to fewer decimals) computes the exact result with a 128-bit intermediate and rounds it once,
///   with an explicit RoundingMode;
/// - parse() and format() convert text such as FIX price fields exactly, without allocation.
///
/// Every operation is noexcept, never allocates or blocks, and takes a bounded number of steps, so
/// all of them are safe on the real-time path. A Decimal is an 8-byte, trivially copyable value with
/// no shared state: each thread uses its own copies, which can be passed through SeqLock, SpscQueue
/// or the other channels.

#ifndef COMMON_NUMERIC_DECIMAL_HPP
#define COMMON_NUMERIC_DECIMAL_HPP

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "common/containers/span.hpp"
#include "common/core/assert.hpp"
#include "common/core/status.hpp"
#include "common/numeric/float.hpp"
#include "common/numeric/integer.hpp"

namespace common {

/// How an operation that drops digits chooses its result.
enum class RoundingMode : std::uint8_t {
    kHalfEven,          ///< To the nearest; a tie goes to the even neighbour (banker's rounding).
    kHalfAwayFromZero,  ///< To the nearest; a tie goes away from zero (commercial rounding).
    kTowardZero,        ///< Truncation: the dropped digits are discarded.
    kFloor,             ///< Toward negative infinity, e.g. a buy price rounded down to the tick.
    kCeiling,           ///< Toward positive infinity, e.g. a sell price rounded up to the tick.
};

namespace detail {
namespace decimal {

/// Largest supported Decimals: 10^18 is the largest power of ten in std::int64_t.
inline constexpr std::uint32_t kMaxDecimals{18U};

/// |INT64_MIN| = 2^63, the largest magnitude a units value can have.
inline constexpr std::uint64_t kMaxMagnitude{std::uint64_t{1U} << 63U};

/// The low 32 bits of a 64-bit word.
inline constexpr std::uint64_t kLow32Mask{0xFFFF'FFFFU};

/// Significand bits of a double, including the implicit leading bit.
inline constexpr std::int32_t kMantissaBits{std::numeric_limits<double>::digits};

/// 10^0 to 10^19: every power of ten below 2^64.
inline constexpr std::array<std::uint64_t, 20U> kPowersOfTen{{1U,
                                                              10U,
                                                              100U,
                                                              1'000U,
                                                              10'000U,
                                                              100'000U,
                                                              1'000'000U,
                                                              10'000'000U,
                                                              100'000'000U,
                                                              1'000'000'000U,
                                                              10'000'000'000U,
                                                              100'000'000'000U,
                                                              1'000'000'000'000U,
                                                              10'000'000'000'000U,
                                                              100'000'000'000'000U,
                                                              1'000'000'000'000'000U,
                                                              10'000'000'000'000'000U,
                                                              100'000'000'000'000'000U,
                                                              1'000'000'000'000'000'000U,
                                                              10'000'000'000'000'000'000U}};

/// The decimal digit characters, indexed by value. Text is converted through this table, so the
/// numerical value of a character is never used (MISRA C++:2023 Rule 7.0.3).
inline constexpr std::array<char, 10U> kDigitChars{{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'}};

/// What digit_value() returns for a character that is not a decimal digit.
inline constexpr std::uint32_t kNotADigit{10U};

/// Unsigned 128-bit integer: high * 2^64 + low. Portable replacement for the compiler-specific
/// 128-bit types, which -Wpedantic rejects.
struct UInt128 final {
    std::uint64_t high{0U};
    std::uint64_t low{0U};
};

/// Where the part dropped by a division lies relative to half of the divisor. This is all that a
/// rounding mode needs to know about it.
enum class Remainder : std::uint8_t {
    kZero,       ///< Nothing was dropped: the quotient is exact.
    kBelowHalf,  ///< 0 < dropped part < 1/2.
    kHalf,       ///< Dropped part == 1/2: a tie.
    kAboveHalf,  ///< 1/2 < dropped part < 1.
};

/// Truncated quotient of a non-negative division and the class of what it dropped.
struct Quotient final {
    UInt128 value{};
    Remainder remainder{Remainder::kZero};
};

/// Exact quotient and remainder of a 128-by-64-bit division.
struct WideDivision final {
    UInt128 quotient{};
    std::uint64_t remainder{0U};
};

/// Digits read by parse_units() so far, and what was wrong with them.
struct ParseState final {
    std::uint64_t magnitude{0U};        ///< Digits read so far, as a number of units.
    std::size_t digits{0U};             ///< Digits read before and after the point.
    std::uint32_t fraction_digits{0U};  ///< Fraction digits included in magnitude.
    bool point{false};                  ///< A '.' was read.
    bool malformed{false};              ///< A character outside the grammar, or a second '.'.
    bool inexact{false};                ///< A non-zero digit beyond the precision.
    bool overflow{false};               ///< The magnitude exceeded 2^63.
};

/// 10^exponent. @pre exponent <= 19
[[nodiscard]] constexpr std::uint64_t power_of_ten(const std::uint32_t exponent) noexcept {
    COMMON_ASSERT(exponent < kPowersOfTen.size());
    return kPowersOfTen[exponent];
}

/// |value| as an unsigned number; also defined for INT64_MIN.
[[nodiscard]] constexpr std::uint64_t magnitude(const std::int64_t value) noexcept {
    return (value < 0) ? (static_cast<std::uint64_t>(-(value + 1)) + 1U) : static_cast<std::uint64_t>(value);
}

/// Full 128-bit product a * b, built from 32-bit halves so that no partial sum can overflow.
[[nodiscard]] constexpr UInt128 multiply_wide(const std::uint64_t a, const std::uint64_t b) noexcept {
    const std::uint64_t a_low{a & kLow32Mask};
    const std::uint64_t a_high{a >> 32U};
    const std::uint64_t b_low{b & kLow32Mask};
    const std::uint64_t b_high{b >> 32U};
    const std::uint64_t low_low{a_low * b_low};
    const std::uint64_t low_high{a_low * b_high};
    const std::uint64_t high_low{a_high * b_low};
    const std::uint64_t high_high{a_high * b_high};
    // Three terms below 2^32 each: the sum is below 2^34.
    const std::uint64_t middle{(low_low >> 32U) + (low_high & kLow32Mask) + (high_low & kLow32Mask)};
    UInt128 product{};
    product.low = ((middle & kLow32Mask) << 32U) | (low_low & kLow32Mask);
    // The full product is below 2^128, so the high word cannot overflow.
    product.high = high_high + (low_high >> 32U) + (high_low >> 32U) + (middle >> 32U);
    return product;
}

/// Exact quotient and remainder of dividend / divisor. @pre divisor != 0
///
/// After one native division of the high word, the rest of the quotient fits in 64 bits. It takes
/// one more native division when the high word divides evenly, two when divisor < 2^32 (32 bits at
/// a time), and otherwise a shift-subtract loop of exactly 64 iterations.
[[nodiscard]] constexpr WideDivision divide_wide(const UInt128 dividend, const std::uint64_t divisor) noexcept {
    COMMON_ASSERT(divisor != 0U);
    WideDivision result{};
    result.quotient.high = dividend.high / divisor;
    const std::uint64_t carried{dividend.high % divisor};
    if (carried == 0U) {
        result.quotient.low = dividend.low / divisor;
        result.remainder = dividend.low % divisor;
    } else if (divisor <= kLow32Mask) {
        // carried < divisor < 2^32, so each partial dividend below is less than 2^64.
        const std::uint64_t upper{(carried << 32U) | (dividend.low >> 32U)};
        const std::uint64_t lower{((upper % divisor) << 32U) | (dividend.low & kLow32Mask)};
        result.quotient.low = ((upper / divisor) << 32U) | (lower / divisor);
        result.remainder = lower % divisor;
    } else {
        std::uint64_t remainder{carried};
        std::uint64_t quotient{0U};
        for (std::uint32_t bit{64U}; bit > 0U; --bit) {
            // remainder = 2 * remainder + next bit, reduced below divisor. Comparing remainder + bit
            // with divisor - remainder avoids the 65-bit intermediate.
            const std::uint64_t next{(dividend.low >> (bit - 1U)) & 1U};
            const std::uint64_t gap{divisor - remainder};
            const std::uint64_t raised{remainder + next};
            quotient <<= 1U;
            if (raised >= gap) {
                remainder = raised - gap;
                quotient |= 1U;
            } else {
                remainder += raised;
            }
        }
        result.quotient.low = quotient;
        result.remainder = remainder;
    }
    return result;
}

/// Position of @p remainder relative to half of @p divisor. @pre remainder < divisor
[[nodiscard]] constexpr Remainder classify(const std::uint64_t remainder, const std::uint64_t divisor) noexcept {
    Remainder result{Remainder::kZero};
    if (remainder != 0U) {
        // remainder versus divisor - remainder, which unlike 2 * remainder cannot overflow.
        const std::uint64_t rest{divisor - remainder};
        if (remainder < rest) {
            result = Remainder::kBelowHalf;
        } else if (remainder == rest) {
            result = Remainder::kHalf;
        } else {
            result = Remainder::kAboveHalf;
        }
    }
    return result;
}

/// value >> shift; zero when shift >= 128.
[[nodiscard]] constexpr UInt128 shift_right(const UInt128 value, const std::uint32_t shift) noexcept {
    UInt128 result{};
    if (shift == 0U) {
        result = value;
    } else if (shift < 64U) {
        const std::uint64_t moved{value.high & ((std::uint64_t{1U} << shift) - 1U)};
        result.high = value.high >> shift;
        result.low = (value.low >> shift) | (moved << (64U - shift));
    } else if (shift < 128U) {
        result.low = value.high >> (shift - 64U);
    } else {
        // Every bit is shifted out: the result stays zero.
    }
    return result;
}

/// @return true if any of the lowest @p count bits of @p value is set.
[[nodiscard]] constexpr bool any_low_bit(const UInt128 value, const std::uint32_t count) noexcept {
    bool set{false};
    if (count < 64U) {
        set = (value.low & ((std::uint64_t{1U} << count) - 1U)) != 0U;
    } else if (count < 128U) {
        set = (value.low != 0U) || ((value.high & ((std::uint64_t{1U} << (count - 64U)) - 1U)) != 0U);
    } else {
        set = (value.low != 0U) || (value.high != 0U);
    }
    return set;
}

/// value / 2^shift truncated, and the class of the bits shifted out.
[[nodiscard]] constexpr Quotient quotient_by_power_of_two(const UInt128 value, const std::uint32_t shift) noexcept {
    Quotient result{};
    result.value = shift_right(value, shift);
    if (shift > 0U) {
        // The highest dropped bit is worth exactly half; any bit below it is worth less than half.
        const bool half{(shift_right(value, shift - 1U).low & 1U) != 0U};
        const bool below_half{any_low_bit(value, shift - 1U)};
        if (half && below_half) {
            result.remainder = Remainder::kAboveHalf;
        } else if (half) {
            result.remainder = Remainder::kHalf;
        } else if (below_half) {
            result.remainder = Remainder::kBelowHalf;
        } else {
            // Nothing dropped: keep kZero.
        }
    }
    return result;
}

/// @return true if rounding with @p mode moves the magnitude from the truncated quotient up by one.
/// @p negative is the sign of the exact result and @p odd the parity of the truncated quotient.
[[nodiscard]] constexpr bool rounds_away(const RoundingMode mode, const bool negative, const bool odd,
                                         const Remainder remainder) noexcept {
    const bool inexact{remainder != Remainder::kZero};
    bool away{false};
    switch (mode) {
        case RoundingMode::kHalfEven:
            away = (remainder == Remainder::kAboveHalf) || ((remainder == Remainder::kHalf) && odd);
            break;
        case RoundingMode::kHalfAwayFromZero:
            away = (remainder == Remainder::kAboveHalf) || (remainder == Remainder::kHalf);
            break;
        case RoundingMode::kTowardZero:
            away = false;
            break;
        case RoundingMode::kFloor:
            away = inexact && negative;
            break;
        case RoundingMode::kCeiling:
            away = inexact && (!negative);
            break;
    }
    return away;
}

/// Applies a sign to a magnitude. @return the value, or kOverflow outside [INT64_MIN, INT64_MAX].
[[nodiscard]] constexpr Result<std::int64_t> to_signed(const std::uint64_t value, const bool negative) noexcept {
    Result<std::int64_t> result{Result<std::int64_t>::failure(Status::kOverflow)};
    if (value < kMaxMagnitude) {
        const std::int64_t positive{static_cast<std::int64_t>(value)};
        result = Result<std::int64_t>::success(negative ? -positive : positive);
    } else if (negative && (value == kMaxMagnitude)) {
        result = Result<std::int64_t>::success(std::numeric_limits<std::int64_t>::min());
    } else {
        // Above INT64_MAX or below INT64_MIN: keep kOverflow.
    }
    return result;
}

/// Rounds @p quotient with @p mode and applies the sign. @return the value, or kOverflow.
[[nodiscard]] constexpr Result<std::int64_t> round_quotient(const Quotient quotient, const bool negative,
                                                            const RoundingMode mode) noexcept {
    Result<std::int64_t> result{Result<std::int64_t>::failure(Status::kOverflow)};
    const std::uint64_t truncated{quotient.value.low};
    // A quotient of 2^64 or more is out of range however it is rounded.
    if (quotient.value.high == 0U) {
        if (!rounds_away(mode, negative, (truncated & 1U) != 0U, quotient.remainder)) {
            result = to_signed(truncated, negative);
        } else if (truncated < kMaxMagnitude) {
            result = to_signed(truncated + 1U, negative);
        } else {
            // truncated + 1 exceeds 2^63: out of range for either sign, keep kOverflow.
        }
    }
    return result;
}

/// dividend / divisor rounded with @p mode, with the sign applied. @return the value, or kOverflow.
/// @pre divisor != 0
[[nodiscard]] constexpr Result<std::int64_t> divide_rounded(const UInt128 dividend, const std::uint64_t divisor,
                                                            const bool negative, const RoundingMode mode) noexcept {
    const WideDivision division{divide_wide(dividend, divisor)};
    Quotient quotient{};
    quotient.value = division.quotient;
    quotient.remainder = classify(division.remainder, divisor);
    return round_quotient(quotient, negative, mode);
}

/// value * 2^shift with the sign applied. @return the value, or kOverflow.
[[nodiscard]] constexpr Result<std::int64_t> shift_left(const UInt128 value, const std::uint32_t shift,
                                                        const bool negative) noexcept {
    Result<std::int64_t> result{Result<std::int64_t>::failure(Status::kOverflow)};
    if ((value.high == 0U) && (value.low == 0U)) {
        result = Result<std::int64_t>::success(0);
    } else if ((value.high == 0U) && (shift < 64U) && (value.low <= (kMaxMagnitude >> shift))) {
        result = to_signed(value.low << shift, negative);
    } else {
        // At least 2^64: keep kOverflow.
    }
    return result;
}

/// Exact binary value of @p value times @p scale, rounded with @p mode.
/// @return the units, kInvalidArgument for NaN or an infinity, or kOverflow.
[[nodiscard]] inline Result<std::int64_t> units_from_double(const double value, const std::uint64_t scale,
                                                           const RoundingMode mode) noexcept {
    Result<std::int64_t> result{Result<std::int64_t>::failure(Status::kInvalidArgument)};
    if (std::isfinite(value)) {
        // |value| = fraction * 2^exponent with fraction in [0.5, 1) (or 0), so mantissa = fraction * 2^53
        // is an integer below 2^53 and |value| = mantissa * 2^(exponent - 53) exactly.
        std::int32_t exponent{0};
        const double fraction{std::frexp(std::fabs(value), &exponent)};
        const std::uint64_t mantissa{saturating_cast<std::uint64_t>(std::ldexp(fraction, kMantissaBits))};
        const std::int32_t binary_exponent{exponent - kMantissaBits};
        const bool negative{value < 0.0};
        const UInt128 product{multiply_wide(mantissa, scale)};  // Exact: below 2^53 * 10^18 < 2^113.
        if (binary_exponent >= 0) {
            result = shift_left(product, static_cast<std::uint32_t>(binary_exponent), negative);
        } else {
            const Quotient quotient{quotient_by_power_of_two(product, static_cast<std::uint32_t>(-binary_exponent))};
            result = round_quotient(quotient, negative, mode);
        }
    }
    return result;
}

/// @return the value of the decimal digit @p character, or kNotADigit.
[[nodiscard]] constexpr std::uint32_t digit_value(const char character) noexcept {
    std::uint32_t value{kNotADigit};
    for (std::uint32_t digit{0U}; digit < 10U; ++digit) {
        if (kDigitChars[digit] == character) {
            value = digit;
        }
    }
    return value;
}

/// magnitude = magnitude * 10 + digit, or sets overflow when the result would exceed 2^63.
constexpr void append_digit(ParseState& state, const std::uint64_t digit) noexcept {
    if (state.overflow || (state.magnitude > ((kMaxMagnitude - digit) / 10U))) {
        state.overflow = true;
    } else {
        state.magnitude = (state.magnitude * 10U) + digit;
    }
}

/// Consumes one character after the optional sign; @p decimals is the precision of the result.
constexpr void parse_character(ParseState& state, const char character, const std::uint32_t decimals) noexcept {
    const std::uint32_t digit{digit_value(character)};
    if (digit != kNotADigit) {
        ++state.digits;
        if (!state.point) {
            append_digit(state, digit);
        } else if (state.fraction_digits < decimals) {
            ++state.fraction_digits;
            append_digit(state, digit);
        } else if (digit != 0U) {
            state.inexact = true;
        } else {
            // A zero beyond the precision does not change the value.
        }
    } else if ((character == '.') && (!state.point)) {
        state.point = true;
    } else {
        state.malformed = true;
    }
}

/// Parses decimal text into a number of units of 10^-decimals; see Decimal::parse().
/// @pre decimals <= kMaxDecimals
[[nodiscard]] constexpr Result<std::int64_t> parse_units(const Span<const char> text,
                                                         const std::uint32_t decimals) noexcept {
    COMMON_ASSERT(decimals <= kMaxDecimals);
    bool negative{false};
    std::size_t start{0U};
    if (!text.empty()) {
        if (text[0U] == '-') {
            negative = true;
            start = 1U;
        } else if (text[0U] == '+') {
            start = 1U;
        } else {
            // No sign.
        }
    }
    ParseState state{};
    for (std::size_t index{start}; index < text.size(); ++index) {
        parse_character(state, text[index], decimals);
    }
    // Missing fraction digits are zeros.
    for (std::uint32_t digit{state.fraction_digits}; digit < decimals; ++digit) {
        append_digit(state, 0U);
    }
    Result<std::int64_t> result{Result<std::int64_t>::failure(Status::kInvalidArgument)};
    if (state.malformed || state.inexact || (state.digits == 0U)) {
        // Not a number, or not exactly representable: keep kInvalidArgument.
    } else if (state.overflow) {
        result = Result<std::int64_t>::failure(Status::kOverflow);
    } else {
        result = to_signed(state.magnitude, negative);
    }
    return result;
}

/// Number of decimal digits of @p value (1 for 0).
[[nodiscard]] constexpr std::size_t digit_count(const std::uint64_t value) noexcept {
    std::size_t count{1U};
    for (std::size_t exponent{1U}; exponent < kPowersOfTen.size(); ++exponent) {
        if (value >= kPowersOfTen[exponent]) {
            count = exponent + 1U;
        }
    }
    return count;
}

/// Writes the lowest digits.size() decimal digits of @p value into @p digits, most significant first.
constexpr void write_digits(const Span<char> digits, const std::uint64_t value) noexcept {
    std::uint64_t rest{value};
    for (std::size_t position{digits.size()}; position > 0U; --position) {
        digits[position - 1U] = kDigitChars[rest % 10U];
        rest /= 10U;
    }
}

/// Formats @p units of 10^-decimals into @p out; see Decimal::format().
/// @pre decimals <= kMaxDecimals
[[nodiscard]] constexpr Result<std::size_t> format_units(const Span<char> out, const std::int64_t units,
                                                         const std::uint32_t decimals) noexcept {
    COMMON_ASSERT(decimals <= kMaxDecimals);
    const std::uint64_t value{magnitude(units)};
    const std::uint64_t scale{power_of_ten(decimals)};
    const std::uint64_t integer_part{value / scale};
    const std::size_t sign_size{(units < 0) ? 1U : 0U};
    const std::size_t integer_size{digit_count(integer_part)};
    const std::size_t fraction_size{(decimals > 0U) ? (static_cast<std::size_t>(decimals) + 1U) : 0U};
    const std::size_t size{sign_size + integer_size + fraction_size};
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kOutOfRange)};
    if (out.size() >= size) {
        if (units < 0) {
            out[0U] = '-';
        }
        write_digits(out.subspan(sign_size, integer_size), integer_part);
        if (decimals > 0U) {
            out[sign_size + integer_size] = '.';
            write_digits(out.subspan(sign_size + integer_size + 1U, decimals), value % scale);
        }
        result = Result<std::size_t>::success(size);
    }
    return result;
}

}  // namespace decimal
}  // namespace detail

/// A decimal number with @p Decimals digits after the point: value = units() * 10^-Decimals.
///
/// The range is that of the std::int64_t units, about +-9.22 * 10^(18 - Decimals), e.g.
/// +-922'337'203'685'477.5807 for Decimal<4>. Scales never mix implicitly: add, subtract, the
/// comparisons and round_to_multiple() need equal Decimals; multiply() accepts any scale and rounds
/// to this one; rescale() converts explicitly.
///
/// @code
///   using Price = common::Decimal<4U>;     // steps of 0.0001
///   using Quantity = common::Decimal<8U>;  // steps of 0.00000001
///   const common::Result<Price> price{Price::parse(price_text)};            // e.g. "101.25"
///   const common::Result<Quantity> quantity{Quantity::parse(quantity_text)};  // e.g. "0.5"
///   if (price.has_value() && quantity.has_value()) {
///       // 50.6250: the exact product 50.625, rounded once to the price's scale.
///       const common::Result<Price> notional{
///           price.value().multiply(quantity.value(), common::RoundingMode::kHalfEven)};
///   }
/// @endcode
template <std::uint32_t Decimals>
class Decimal final {
    static_assert(Decimals <= detail::decimal::kMaxDecimals,
                  "Decimals must be at most 18, so that 10^Decimals fits in std::int64_t");

public:
    /// The most characters format() writes: a sign, 19 digits and the point.
    static constexpr std::size_t kMaxFormattedSize{(Decimals > 0U) ? 21U : 20U};

    /// Zero.
    constexpr Decimal() noexcept = default;

    /// The value units * 10^-Decimals. Never fails.
    [[nodiscard]] static constexpr Decimal from_units(const std::int64_t units) noexcept {
        Decimal result{};
        result.units_ = units;
        return result;
    }

    /// The integer @p value. @return the value, or kOverflow if it is out of range.
    [[nodiscard]] static constexpr Result<Decimal> from_integer(const std::int64_t value) noexcept {
        return from_scaled(checked_mul(value, kScale));
    }

    /// Converts the exact binary value of @p value, rounded to Decimals digits with @p mode.
    ///
    /// The binary value is rarely the decimal written in source code or text: 1.005 is stored as
    /// 1.00499999999999989..., so from_double(1.005, kHalfAwayFromZero) gives 1.00 for two decimals.
    /// Use it for values that are binary by nature (sensor readings, model outputs); parse() is the
    /// exact path for text.
    /// @return the value; kInvalidArgument for NaN or an infinity; kOverflow if out of range.
    [[nodiscard]] static Result<Decimal> from_double(const double value, const RoundingMode mode) noexcept {
        return from_scaled(detail::decimal::units_from_double(value, kUnsignedScale, mode));
    }

    /// Parses text such as a FIX price field: an optional '+' or '-', then digits with at most one
    /// '.', and at least one digit ("42", "-0.5", ".5", "42.", "0042.50"). No exponent, whitespace or
    /// digit separator is accepted. Parsing is exact and never rounds: digits after the point beyond
    /// Decimals are accepted only if they are zeros ("1.2500" for Decimals == 2). O(text.size()).
    /// @return the value; kInvalidArgument if the text is malformed or has a non-zero digit beyond
    ///         Decimals; otherwise kOverflow if the value is out of range.
    [[nodiscard]] static constexpr Result<Decimal> parse(const Span<const char> text) noexcept {
        return from_scaled(detail::decimal::parse_units(text, Decimals));
    }

    /// Writes the value as text, without a terminating null character: a '-' if negative, the
    /// integer digits (at least one), and for Decimals > 0 a '.' followed by exactly Decimals digits
    /// ("-12.3400" for Decimal<4>, "7" for Decimal<0>). At most kMaxFormattedSize characters.
    /// @return the number of characters written, or kOutOfRange if @p out is too small (nothing is
    ///         written then).
    [[nodiscard]] constexpr Result<std::size_t> format(const Span<char> out) const noexcept {
        return detail::decimal::format_units(out, units_, Decimals);
    }

    /// The value in units of 10^-Decimals.
    [[nodiscard]] constexpr std::int64_t units() const noexcept {
        return units_;
    }

    /// 10^Decimals, the number of units in 1.
    [[nodiscard]] static constexpr std::int64_t scale() noexcept {
        return kScale;
    }

    /// Approximate value, e.g. for display or a floating-point model; never feed it back into decimal
    /// arithmetic. It is the double nearest to the exact value when |units()| <= 2^53.
    [[nodiscard]] double to_double() const noexcept {
        return static_cast<double>(units_) / static_cast<double>(kScale);
    }

    /// @return this + other, or kOverflow.
    [[nodiscard]] constexpr Result<Decimal> add(const Decimal other) const noexcept {
        return from_scaled(checked_add(units_, other.units_));
    }

    /// @return this - other, or kOverflow.
    [[nodiscard]] constexpr Result<Decimal> subtract(const Decimal other) const noexcept {
        return from_scaled(checked_sub(units_, other.units_));
    }

    /// @return -this, or kOverflow when units() == INT64_MIN.
    [[nodiscard]] constexpr Result<Decimal> negate() const noexcept {
        return from_scaled(checked_sub(std::int64_t{0}, units_));
    }

    /// Exact product with an integer, e.g. a lot size times a number of lots.
    /// @return this * factor, or kOverflow.
    [[nodiscard]] constexpr Result<Decimal> multiply(const std::int64_t factor) const noexcept {
        return from_scaled(checked_mul(units_, factor));
    }

    /// Product with a value of any scale, rounded to this scale with @p mode, e.g. a price times a
    /// fractional quantity gives a notional at the price's scale. The exact product is computed in
    /// 128 bits and rounded once.
    /// @return the rounded product, or kOverflow if it is out of range.
    template <std::uint32_t OtherDecimals>
    [[nodiscard]] constexpr Result<Decimal> multiply(const Decimal<OtherDecimals> other,
                                                     const RoundingMode mode) const noexcept {
        const bool negative{(units_ < 0) != (other.units() < 0)};
        const detail::decimal::UInt128 product{detail::decimal::multiply_wide(
            detail::decimal::magnitude(units_), detail::decimal::magnitude(other.units()))};
        return from_scaled(
            detail::decimal::divide_rounded(product, detail::decimal::power_of_ten(OtherDecimals), negative, mode));
    }

    /// Quotient by an integer, rounded once with @p mode, e.g. an amount split into equal parts.
    /// @return the rounded quotient; kInvalidArgument if divisor == 0; kOverflow for
    ///         units() == INT64_MIN and divisor == -1.
    [[nodiscard]] constexpr Result<Decimal> divide(const std::int64_t divisor, const RoundingMode mode) const noexcept {
        Result<Decimal> result{Result<Decimal>::failure(Status::kInvalidArgument)};
        if (divisor != 0) {
            const bool negative{(units_ < 0) != (divisor < 0)};
            detail::decimal::UInt128 dividend{};
            dividend.low = detail::decimal::magnitude(units_);
            result = from_scaled(
                detail::decimal::divide_rounded(dividend, detail::decimal::magnitude(divisor), negative, mode));
        }
        return result;
    }

    /// Rounds to a multiple of @p step with @p mode, e.g. a price to the tick size of an instrument.
    /// @return the multiple; kInvalidArgument if step <= 0; kOverflow if the multiple is out of range.
    [[nodiscard]] constexpr Result<Decimal> round_to_multiple(const Decimal step,
                                                              const RoundingMode mode) const noexcept {
        Result<Decimal> result{Result<Decimal>::failure(Status::kInvalidArgument)};
        if (step.units_ > 0) {
            detail::decimal::UInt128 dividend{};
            dividend.low = detail::decimal::magnitude(units_);
            const Result<std::int64_t> count{detail::decimal::divide_rounded(
                dividend, static_cast<std::uint64_t>(step.units_), units_ < 0, mode)};
            result = count.has_value() ? from_scaled(checked_mul(count.value(), step.units_)) : from_scaled(count);
        }
        return result;
    }

    /// Converts to @p NewDecimals digits. More digits is exact (@p mode does not matter) and can
    /// overflow; fewer digits rounds with @p mode and never overflows.
    /// @return the converted value, or kOverflow.
    template <std::uint32_t NewDecimals>
    [[nodiscard]] constexpr Result<Decimal<NewDecimals>> rescale(const RoundingMode mode) const noexcept {
        Result<std::int64_t> scaled{Result<std::int64_t>::success(units_)};
        if constexpr (NewDecimals > Decimals) {
            static_cast<void>(mode);  // Exact: adding digits never rounds.
            const std::uint64_t factor{detail::decimal::power_of_ten(NewDecimals - Decimals)};
            scaled = checked_mul(units_, static_cast<std::int64_t>(factor));
        } else if constexpr (NewDecimals < Decimals) {
            detail::decimal::UInt128 dividend{};
            dividend.low = detail::decimal::magnitude(units_);
            scaled = detail::decimal::divide_rounded(dividend, detail::decimal::power_of_ten(Decimals - NewDecimals),
                                                     units_ < 0, mode);
        } else {
            static_cast<void>(mode);  // Same scale: the value is unchanged.
        }
        return scaled.has_value()
                   ? Result<Decimal<NewDecimals>>::success(Decimal<NewDecimals>::from_units(scaled.value()))
                   : Result<Decimal<NewDecimals>>::failure(scaled.status());
    }

    [[nodiscard]] constexpr bool operator==(const Decimal other) const noexcept {
        return units_ == other.units_;
    }

    [[nodiscard]] constexpr bool operator!=(const Decimal other) const noexcept {
        return units_ != other.units_;
    }

    [[nodiscard]] constexpr bool operator<(const Decimal other) const noexcept {
        return units_ < other.units_;
    }

    [[nodiscard]] constexpr bool operator<=(const Decimal other) const noexcept {
        return units_ <= other.units_;
    }

    [[nodiscard]] constexpr bool operator>(const Decimal other) const noexcept {
        return units_ > other.units_;
    }

    [[nodiscard]] constexpr bool operator>=(const Decimal other) const noexcept {
        return units_ >= other.units_;
    }

private:
    static constexpr std::uint64_t kUnsignedScale{detail::decimal::kPowersOfTen[Decimals]};
    static constexpr std::int64_t kScale{static_cast<std::int64_t>(kUnsignedScale)};

    /// Wraps a units result.
    [[nodiscard]] static constexpr Result<Decimal> from_scaled(const Result<std::int64_t>& scaled) noexcept {
        return scaled.has_value() ? Result<Decimal>::success(from_units(scaled.value()))
                                  : Result<Decimal>::failure(scaled.status());
    }

    std::int64_t units_{0};
};

}  // namespace common

#endif  // COMMON_NUMERIC_DECIMAL_HPP
