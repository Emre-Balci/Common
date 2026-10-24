/// @file text.hpp
/// @brief Matching, parsing and formatting of text-based protocol messages without allocation.
///
/// Many devices speak line-oriented ASCII protocols: modems, GNSS receivers, diagnostic adapters
/// such as ELM327-compatible OBD interfaces. Their responses mix keywords ("OK", "NO DATA"),
/// decimal numbers and hexadecimal bytes ("41 0C 1A F8"). This header provides what parsing and
/// building such messages needs:
/// - classification (is_space, is_decimal_digit, is_hex_digit) and trimming;
/// - comparison of a received text with a pattern: equals, starts_with, ends_with, find, contains;
/// - parse_unsigned, parse_hex and decode_hex_bytes, which reject anything that is not a valid
///   number and report overflow instead of wrapping;
/// - format_unsigned, format_hex and format_hex_bytes, which write into a caller-supplied buffer,
///   and append_unsigned, append_hex and append_hex_bytes for a StaticString.
///
/// Received text is passed as a Span<const char> (e.g. a frame of a DelimitedFramer or a line of a
/// LineReader); patterns are std::string_view, so string literals can be passed directly. Only
/// ASCII is interpreted; the functions do not depend on the C locale.
///
/// Every function is noexcept, never allocates or blocks, and loops at most over its input and
/// pattern, so all of them are safe on the real-time path.

#ifndef COMMON_PROTOCOL_TEXT_HPP
#define COMMON_PROTOCOL_TEXT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/core/status.hpp"

namespace common {

namespace detail {
namespace text {

template <typename T>
inline constexpr bool kIsUnsignedInteger{std::is_integral<T>::value && std::is_unsigned<T>::value &&
                                         (!std::is_same<T, bool>::value)};

// Characters are converted to and from digits through the tables below, and are otherwise only
// compared for equality (std::string_view::find() included), so the numerical value of a character
// is never used (MISRA C++:2023 Rule 7.0.3).

/// Decimal digits, indexed by their value.
inline constexpr std::string_view kDecimalDigits{"0123456789"};

/// Upper-case hexadecimal digits, indexed by their value.
inline constexpr std::string_view kHexDigits{"0123456789ABCDEF"};

/// Lower-case hexadecimal digits, indexed by their value.
inline constexpr std::string_view kLowerHexDigits{"0123456789abcdef"};

/// The white-space characters is_space() accepts.
inline constexpr std::string_view kSpaces{" \t\n\v\f\r"};

/// Characters needed for any std::uint64_t: 20 decimal or 16 hexadecimal digits.
inline constexpr std::size_t kMaxDigits{20U};

/// Bits per hexadecimal digit.
inline constexpr std::uint32_t kBitsPerHexDigit{4U};

/// @return the hexadecimal digit for the low four bits of @p value.
[[nodiscard]] constexpr char hex_digit(const std::uint64_t value) noexcept {
    return kHexDigits[value & 0xFU];
}

/// @return true if @p character occurs in @p characters.
[[nodiscard]] constexpr bool is_one_of(const std::string_view characters, const char character) noexcept {
    return characters.find(character) != std::string_view::npos;
}

}  // namespace text
}  // namespace detail

/// @return true for the ASCII white-space characters: space, \\t, \\n, \\v, \\f and \\r.
[[nodiscard]] constexpr bool is_space(const char character) noexcept {
    return detail::text::is_one_of(detail::text::kSpaces, character);
}

[[nodiscard]] constexpr bool is_decimal_digit(const char character) noexcept {
    return detail::text::is_one_of(detail::text::kDecimalDigits, character);
}

/// @return true for 0-9, a-f and A-F.
[[nodiscard]] constexpr bool is_hex_digit(const char character) noexcept {
    return detail::text::is_one_of(detail::text::kHexDigits, character) ||
           detail::text::is_one_of(detail::text::kLowerHexDigits, character);
}

/// @return the value (0 to 15) of the hexadecimal digit @p character; kInvalidArgument if it is not one.
[[nodiscard]] constexpr Result<std::uint8_t> hex_digit_value(const char character) noexcept {
    // A digit's position in its table is its value.
    std::size_t value{detail::text::kHexDigits.find(character)};
    if (value == std::string_view::npos) {
        value = detail::text::kLowerHexDigits.find(character);
    }
    return (value != std::string_view::npos) ? Result<std::uint8_t>::success(static_cast<std::uint8_t>(value))
                                             : Result<std::uint8_t>::failure(Status::kInvalidArgument);
}

/// @return @p text without leading and trailing white space (see is_space()).
[[nodiscard]] constexpr Span<const char> trim(const Span<const char> text) noexcept {
    std::size_t begin{0U};
    while ((begin < text.size()) && is_space(text[begin])) {
        ++begin;
    }
    std::size_t end{text.size()};
    while ((end > begin) && is_space(text[end - 1U])) {
        --end;
    }
    return text.subspan(begin, end - begin);
}

/// @return true if @p text and @p pattern hold the same characters (case-sensitive).
[[nodiscard]] constexpr bool equals(const Span<const char> text, const std::string_view pattern) noexcept {
    bool same{text.size() == pattern.size()};
    for (std::size_t index{0U}; same && (index < text.size()); ++index) {
        same = text[index] == pattern[index];
    }
    return same;
}

/// @return true if @p text begins with @p pattern; always true for an empty pattern.
[[nodiscard]] constexpr bool starts_with(const Span<const char> text, const std::string_view pattern) noexcept {
    return (pattern.size() <= text.size()) && equals(text.first(pattern.size()), pattern);
}

/// @return true if @p text ends with @p pattern; always true for an empty pattern.
[[nodiscard]] constexpr bool ends_with(const Span<const char> text, const std::string_view pattern) noexcept {
    return (pattern.size() <= text.size()) && equals(text.last(pattern.size()), pattern);
}

/// @return the position of the first occurrence of @p pattern in @p text (0 for an empty
///         pattern); kNotFound if there is none. Takes at most text.size() * pattern.size() steps.
[[nodiscard]] constexpr Result<std::size_t> find(const Span<const char> text, const std::string_view pattern) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kNotFound)};
    if (pattern.size() <= text.size()) {
        const std::size_t last_start{text.size() - pattern.size()};
        for (std::size_t start{0U}; start <= last_start; ++start) {
            if (equals(text.subspan(start, pattern.size()), pattern)) {
                result = Result<std::size_t>::success(start);
                break;
            }
        }
    }
    return result;
}

/// @return true if @p pattern occurs in @p text.
[[nodiscard]] constexpr bool contains(const Span<const char> text, const std::string_view pattern) noexcept {
    return find(text, pattern).has_value();
}

/// Parses all of @p text as a decimal number, e.g. "1200". Leading zeros are accepted; signs,
/// white space and any other character are not (trim() the text first if needed).
/// @return kInvalidArgument for an empty text or a character that is not a digit; kOverflow if the
///         value does not fit in T.
template <typename T>
[[nodiscard]] constexpr Result<T> parse_unsigned(const Span<const char> text) noexcept {
    static_assert(detail::text::kIsUnsignedInteger<T>, "T must be an unsigned integer type");
    constexpr T kMax{std::numeric_limits<T>::max()};
    constexpr T kBase{10U};
    Status status{text.empty() ? Status::kInvalidArgument : Status::kOk};
    T value{0U};
    for (std::size_t index{0U}; is_ok(status) && (index < text.size()); ++index) {
        // A digit's position in the table is its value.
        const std::size_t position{detail::text::kDecimalDigits.find(text[index])};
        if (position == std::string_view::npos) {
            status = Status::kInvalidArgument;
        } else {
            const T digit{static_cast<T>(position)};
            if (value > static_cast<T>((kMax - digit) / kBase)) {
                status = Status::kOverflow;
            } else {
                value = static_cast<T>((value * kBase) + digit);
            }
        }
    }
    return is_ok(status) ? Result<T>::success(value) : Result<T>::failure(status);
}

/// Parses all of @p text as a hexadecimal number in either case, e.g. "7E8" or "1af8", without a
/// "0x" prefix. Leading zeros are accepted, so "00FF" fits in a std::uint8_t.
/// @return kInvalidArgument for an empty text or a character that is not a hexadecimal digit;
///         kOverflow if the value does not fit in T.
template <typename T>
[[nodiscard]] constexpr Result<T> parse_hex(const Span<const char> text) noexcept {
    static_assert(detail::text::kIsUnsignedInteger<T>, "T must be an unsigned integer type");
    constexpr T kOverflowMask{static_cast<T>(std::uint64_t{0xFU} << ((sizeof(T) * 8U) - 4U))};
    Status status{text.empty() ? Status::kInvalidArgument : Status::kOk};
    T value{0U};
    for (std::size_t index{0U}; is_ok(status) && (index < text.size()); ++index) {
        const Result<std::uint8_t> digit{hex_digit_value(text[index])};
        if (!digit.has_value()) {
            status = Status::kInvalidArgument;
        } else if ((value & kOverflowMask) != 0U) {
            status = Status::kOverflow;  // Shifting in another digit would lose the top bits.
        } else {
            value = static_cast<T>(static_cast<T>(value << detail::text::kBitsPerHexDigit) | digit.value());
        }
    }
    return is_ok(status) ? Result<T>::success(value) : Result<T>::failure(status);
}

/// Decodes hexadecimal byte pairs, e.g. "41 0C 1A F8" or "410C1AF8", into @p out. White space
/// (see is_space()) may separate bytes but not split one, so "4 10C" is rejected.
/// @return the number of bytes written to @p out (0 for a text that is empty or all white space);
///         kInvalidArgument for a character that is neither a hexadecimal digit nor white space, or
///         an odd digit; kOutOfRange if @p out is too small (@p out then holds the bytes that fit).
[[nodiscard]] constexpr Result<std::size_t> decode_hex_bytes(const Span<const char> text,
                                                             const Span<std::uint8_t> out) noexcept {
    Status status{Status::kOk};
    std::size_t count{0U};
    std::size_t index{0U};
    while (is_ok(status) && (index < text.size())) {
        if (is_space(text[index])) {
            ++index;
        } else if ((index + 1U) >= text.size()) {
            status = Status::kInvalidArgument;  // A single digit at the end.
        } else {
            const Result<std::uint8_t> high{hex_digit_value(text[index])};
            const Result<std::uint8_t> low{hex_digit_value(text[index + 1U])};
            if ((!high.has_value()) || (!low.has_value())) {
                status = Status::kInvalidArgument;
            } else if (count >= out.size()) {
                status = Status::kOutOfRange;
            } else {
                out[count] = static_cast<std::uint8_t>(
                    (static_cast<std::uint32_t>(high.value()) << detail::text::kBitsPerHexDigit) |
                    static_cast<std::uint32_t>(low.value()));
                ++count;
                index += 2U;
            }
        }
    }
    return is_ok(status) ? Result<std::size_t>::success(count) : Result<std::size_t>::failure(status);
}

/// Writes @p value in decimal, without leading zeros, to the start of @p out. No NUL is written.
/// @return the number of characters written; kOutOfRange if @p out is too small (nothing is written).
template <typename T>
[[nodiscard]] constexpr Result<std::size_t> format_unsigned(const T value, const Span<char> out) noexcept {
    static_assert(detail::text::kIsUnsignedInteger<T>, "T must be an unsigned integer type");
    std::uint64_t rest{value};
    std::size_t length{0U};
    std::array<char, detail::text::kMaxDigits> reversed{};
    do {
        reversed[length] = detail::text::kDecimalDigits[rest % 10U];
        rest /= 10U;
        ++length;
    } while ((rest != 0U) && (length < reversed.size()));
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kOutOfRange)};
    if (length <= out.size()) {
        for (std::size_t index{0U}; index < length; ++index) {
            out[index] = reversed[length - 1U - index];
        }
        result = Result<std::size_t>::success(length);
    }
    return result;
}

/// Writes @p value as exactly @p digits upper-case hexadecimal digits, zero-padded, to the start of
/// @p out, e.g. 0x7E0 with 3 digits as "7E0". No NUL is written.
/// @return the number of characters written (@p digits); kInvalidArgument if @p digits is 0 or more
///         than T has (2 * sizeof(T)); kOutOfRange if @p value needs more digits or @p out is too
///         small (nothing is written).
template <typename T>
[[nodiscard]] constexpr Result<std::size_t> format_hex(const T value, const std::size_t digits,
                                                       const Span<char> out) noexcept {
    static_assert(detail::text::kIsUnsignedInteger<T>, "T must be an unsigned integer type");
    constexpr std::size_t kMaxHexDigits{sizeof(T) * 2U};
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInvalidArgument)};
    if ((digits > 0U) && (digits <= kMaxHexDigits)) {
        const std::uint64_t wide{value};
        const bool fits{(digits == 16U) || ((wide >> (digits * detail::text::kBitsPerHexDigit)) == 0U)};
        if (fits && (digits <= out.size())) {
            for (std::size_t index{0U}; index < digits; ++index) {
                const std::size_t shift{(digits - 1U - index) * detail::text::kBitsPerHexDigit};
                out[index] = detail::text::hex_digit(wide >> shift);
            }
            result = Result<std::size_t>::success(digits);
        } else {
            result = Result<std::size_t>::failure(Status::kOutOfRange);
        }
    }
    return result;
}

/// Writes @p bytes as upper-case hexadecimal pairs to the start of @p out, each pair after the
/// first preceded by @p separator, e.g. "01 0C" with ' '. Pass '\\0' to write no separator ("010C").
/// No NUL is written.
/// @return the number of characters written; kOutOfRange if @p out is too small (nothing is written).
[[nodiscard]] constexpr Result<std::size_t> format_hex_bytes(const Span<const std::uint8_t> bytes, const Span<char> out,
                                                             const char separator) noexcept {
    const std::size_t step{(separator == '\0') ? 2U : 3U};
    // n bytes need 2 + (n - 1) * step characters. Dividing instead of multiplying keeps the check
    // free of overflow for any bytes.size().
    const bool fits{bytes.empty() || ((out.size() >= 2U) && ((bytes.size() - 1U) <= ((out.size() - 2U) / step)))};
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kOutOfRange)};
    if (fits) {
        const std::size_t length{bytes.empty() ? 0U : (2U + ((bytes.size() - 1U) * step))};
        std::size_t position{0U};
        for (std::size_t index{0U}; index < bytes.size(); ++index) {
            if ((index > 0U) && (step == 3U)) {
                out[position] = separator;
                ++position;
            }
            out[position] = detail::text::hex_digit(static_cast<std::uint64_t>(bytes[index]) >> 4U);
            out[position + 1U] = detail::text::hex_digit(bytes[index]);
            position += 2U;
        }
        result = Result<std::size_t>::success(length);
    }
    return result;
}

/// Appends @p value in decimal to @p text. @return false (@p text is unchanged) if it does not fit.
template <typename T, std::size_t Capacity>
[[nodiscard]] constexpr bool append_unsigned(StaticString<Capacity>& text, const T value) noexcept {
    std::array<char, detail::text::kMaxDigits> digits{};
    const Result<std::size_t> length{format_unsigned(value, Span<char>{digits})};
    return length.has_value() && text.append(Span<const char>{digits.data(), length.value()});
}

/// Appends @p value as @p digits upper-case hexadecimal digits to @p text (see format_hex()).
/// @return false (@p text is unchanged) if the arguments are invalid or the result does not fit.
template <typename T, std::size_t Capacity>
[[nodiscard]] constexpr bool append_hex(StaticString<Capacity>& text, const T value, const std::size_t digits) noexcept {
    std::array<char, detail::text::kMaxDigits> buffer{};
    const Result<std::size_t> length{format_hex(value, digits, Span<char>{buffer})};
    return length.has_value() && text.append(Span<const char>{buffer.data(), length.value()});
}

/// Appends @p bytes as hexadecimal pairs separated by @p separator to @p text (see
/// format_hex_bytes()). @return false (@p text is unchanged) if the result does not fit.
template <std::size_t Capacity>
[[nodiscard]] constexpr bool append_hex_bytes(StaticString<Capacity>& text, const Span<const std::uint8_t> bytes,
                                              const char separator) noexcept {
    bool appended{true};
    const std::size_t size_before{text.size()};
    for (std::size_t index{0U}; appended && (index < bytes.size()); ++index) {
        if ((index > 0U) && (separator != '\0')) {
            appended = text.append(separator);
        }
        appended = appended && append_hex(text, bytes[index], 2U);
    }
    if (!appended) {
        text.truncate(size_before);  // All or nothing, like the other appends.
    }
    return appended;
}

}  // namespace common

#endif  // COMMON_PROTOCOL_TEXT_HPP
