#include "common/diagnostics/logger.hpp"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/core/assert.hpp"
#include "common/core/status.hpp"
#include "common/numeric/decimal.hpp"
#include "common/numeric/float.hpp"
#include "common/time/clock.hpp"

#if defined(_WIN32)
#include "detail/win32.hpp"
#endif

namespace common {
namespace {

// format_log_record() reads at most this many characters of a format string.
constexpr std::size_t kMaxFormatLength{4096U};

#if !defined(_WIN32)
// An EINTR restart makes no progress; the bound only guards against a signal storm.
constexpr std::uint32_t kMaxWriteRestarts{1000U};
#endif

// Digits are written through this table, so the numerical value of a character is never used
// (MISRA C++:2023 Rule 7.0.3).
constexpr std::array<char, 10U> kDigitChars{{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'}};

// Enough for any std::uint64_t (20 digits).
constexpr std::size_t kMaxDigits{20U};

constexpr std::uint64_t kNanosPerSecond{1'000'000'000U};

// Doubles with a magnitude below this print in fixed notation, larger ones in scientific notation.
constexpr double kFixedLimit{1e15};

// Fractional digits of both notations, and 10 to that power.
constexpr std::uint32_t kFractionDigits{6U};
constexpr std::int64_t kFractionScale{1'000'000};

/// Writes a line into a buffer, keeping the last character of the buffer for the final '\n'.
/// Characters beyond that are dropped, which truncates the line.
class LineWriter final {
public:
    explicit LineWriter(const Span<char> out) noexcept : out_{out}, limit_{out.empty() ? 0U : (out.size() - 1U)} {}

    void put(const char character) noexcept {
        if (length_ < limit_) {
            out_[length_] = character;
            ++length_;
        }
    }

    void put(const std::string_view text) noexcept {
        for (const char character : text) {
            put(character);
        }
    }

    /// Writes @p value in decimal, with leading zeros up to @p min_digits digits (at most 20).
    void put_unsigned(const std::uint64_t value, const std::size_t min_digits) noexcept {
        std::array<char, kMaxDigits> reversed{};
        std::uint64_t rest{value};
        std::size_t count{0U};
        do {
            reversed[count] = kDigitChars[rest % 10U];
            rest /= 10U;
            ++count;
        } while ((rest != 0U) && (count < kMaxDigits));
        while ((count < min_digits) && (count < kMaxDigits)) {
            reversed[count] = '0';
            ++count;
        }
        for (std::size_t index{count}; index > 0U; --index) {
            put(reversed[index - 1U]);
        }
    }

    /// Writes @p value in decimal, with a '-' if it is negative (also for INT64_MIN).
    void put_signed(const std::int64_t value) noexcept {
        if (value < 0) {
            put('-');
            put_unsigned(static_cast<std::uint64_t>(-(value + 1)) + 1U, 1U);
        } else {
            put_unsigned(static_cast<std::uint64_t>(value), 1U);
        }
    }

    /// Writes '.' and the digits of @p fraction (0 to 999'999, i.e. six decimals) without trailing
    /// zeros; nothing at all for 0.
    void put_fraction(const std::int64_t fraction) noexcept {
        if (fraction > 0) {
            std::array<char, kFractionDigits> digits{};
            std::int64_t rest{fraction};
            for (std::size_t index{kFractionDigits}; index > 0U; --index) {
                digits[index - 1U] = kDigitChars[static_cast<std::size_t>(rest % 10)];
                rest /= 10;
            }
            std::size_t used{kFractionDigits};
            while ((used > 1U) && (digits[used - 1U] == '0')) {
                --used;
            }
            put('.');
            for (std::size_t index{0U}; index < used; ++index) {
                put(digits[index]);
            }
        }
    }

    /// Ends the line with '\n'. @return the line length including the '\n'; 0 for an empty buffer.
    [[nodiscard]] std::size_t finish() noexcept {
        std::size_t total{0U};
        if (!out_.empty()) {
            out_[length_] = '\n';
            total = length_ + 1U;
        }
        return total;
    }

private:
    Span<char> out_;
    std::size_t limit_;
    std::size_t length_{0U};
};

// The characters a text argument must not emit: the C0 control characters 0x00 to 0x1F and DEL
// (0x7F). They are matched by equality, so the numerical value of a character is never used
// (MISRA C++:2023 Rule 7.0.3). The explicit length keeps the leading NUL.
constexpr std::string_view kControlCharacters{
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
    "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F\x7F",
    33U};

[[nodiscard]] bool is_control_character(const char character) noexcept {
    return kControlCharacters.find(character) != std::string_view::npos;
}

void put_sanitized(LineWriter& line, const char character) noexcept {
    line.put(is_control_character(character) ? '?' : character);
}

/// |value| < 1e15, not negative: exactly the digits of printf("%.6f") without trailing zeros.
void put_fixed(LineWriter& line, const double magnitude) noexcept {
    // Both parts are exact: trunc() drops only fraction bits, and the subtraction is exact by
    // Sterbenz's lemma (integer_part <= magnitude < 2 * integer_part, or integer_part == 0).
    const double integer_part{std::trunc(magnitude)};
    const double fraction{magnitude - integer_part};
    std::uint64_t whole{saturating_cast<std::uint64_t>(integer_part)};  // Exact: below 1e15.
    // The fraction rounded to six decimals, ties to even like printf. whole * 10^6 is even, so the
    // parity that decides a tie is the fraction's own.
    const Result<Decimal<kFractionDigits>> rounded{
        Decimal<kFractionDigits>::from_double(fraction, RoundingMode::kHalfEven)};
    std::int64_t micros{rounded.has_value() ? rounded.value().units() : 0};
    if (micros >= kFractionScale) {
        ++whole;  // The fraction rounded up to 1.
        micros -= kFractionScale;
    }
    line.put_unsigned(whole, 1U);
    line.put_fraction(micros);
}

/// |value| >= 1e15, finite, not negative: d[.dddddd]e+XX, scaled with double arithmetic.
void put_scientific(LineWriter& line, const double magnitude) noexcept {
    std::int32_t exponent{saturating_cast<std::int32_t>(std::floor(std::log10(magnitude)))};
    double scaled{magnitude / std::pow(10.0, exponent)};
    // log10() may be off by one next to a power of ten.
    if (scaled >= 10.0) {
        scaled /= 10.0;
        ++exponent;
    } else if (scaled < 1.0) {
        scaled *= 10.0;
        --exponent;
    } else {
        // Already in [1, 10).
    }
    std::int64_t digits{saturating_round<std::int64_t>(scaled * static_cast<double>(kFractionScale))};
    if (digits >= (10 * kFractionScale)) {
        digits = kFractionScale;  // 9.9999995 rounded up to 10.
        ++exponent;
    }
    line.put_unsigned(static_cast<std::uint64_t>(digits / kFractionScale), 1U);
    line.put_fraction(digits % kFractionScale);
    line.put("e+");
    line.put_unsigned(static_cast<std::uint64_t>(exponent), 2U);
}

void put_double(LineWriter& line, const double value) noexcept {
    if (std::isnan(value)) {
        line.put("nan");
    } else if (std::isinf(value)) {
        line.put((value < 0.0) ? "-inf" : "inf");
    } else {
        if (std::signbit(value)) {
            line.put('-');  // Also for -0.0 and negative values that round to 0, like printf.
        }
        const double magnitude{std::fabs(value)};
        if (magnitude < kFixedLimit) {
            put_fixed(line, magnitude);
        } else {
            put_scientific(line, magnitude);
        }
    }
}

void put_argument(LineWriter& line, const LogArgument& argument) noexcept {
    switch (argument.kind()) {
        case LogArgument::Kind::kNone:
            line.put("{}");
            break;
        case LogArgument::Kind::kSigned:
            line.put_signed(argument.as_signed());
            break;
        case LogArgument::Kind::kUnsigned:
            line.put_unsigned(argument.as_unsigned(), 1U);
            break;
        case LogArgument::Kind::kDouble:
            put_double(line, argument.as_double());
            break;
        case LogArgument::Kind::kBool:
            line.put(argument.as_bool() ? "true" : "false");
            break;
        case LogArgument::Kind::kChar:
            put_sanitized(line, argument.as_char());
            break;
        case LogArgument::Kind::kText:
            for (const char character : argument.as_text()) {
                put_sanitized(line, character);
            }
            break;
    }
}

/// "<seconds>.<9-digit nanoseconds>", with a '-' before a time before the clock's epoch.
void put_time(LineWriter& line, const TimePoint time) noexcept {
    const std::int64_t nanoseconds{time.time_since_epoch().count()};
    std::uint64_t magnitude{0U};
    if (nanoseconds < 0) {
        line.put('-');
        magnitude = static_cast<std::uint64_t>(-(nanoseconds + 1)) + 1U;  // Also for INT64_MIN.
    } else {
        magnitude = static_cast<std::uint64_t>(nanoseconds);
    }
    line.put_unsigned(magnitude / kNanosPerSecond, 1U);
    line.put('.');
    line.put_unsigned(magnitude % kNanosPerSecond, 9U);
}

/// The format with each "{}" replaced by the next argument; see format_log_record().
void put_message(LineWriter& line, const LogRecord& record) noexcept {
    const char* const format{record.format};
    if (format != nullptr) {
        const std::size_t argument_count{(record.argument_count <= kMaxLogArguments) ? record.argument_count
                                                                                    : kMaxLogArguments};
        std::size_t next_argument{0U};
        std::size_t index{0U};
        // Every round consumes one or two characters, and index never passes kMaxFormatLength.
        while ((index < kMaxFormatLength) && (format[index] != '\0')) {
            const char current{format[index]};
            // The NUL ends the format, so the following character is read only before it.
            const char following{((index + 1U) < kMaxFormatLength) ? format[index + 1U] : '\0'};
            if ((current == '{') && (following == '{')) {
                line.put('{');
                index += 2U;
            } else if ((current == '}') && (following == '}')) {
                line.put('}');
                index += 2U;
            } else if ((current == '{') && (following == '}')) {
                if (next_argument < argument_count) {
                    put_argument(line, record.arguments[next_argument]);
                    ++next_argument;
                } else {
                    line.put("{}");  // No argument left.
                }
                index += 2U;
            } else {
                line.put(current);
                ++index;
            }
        }
    }
}

}  // namespace

std::size_t format_log_record(const LogRecord& record, const Span<char> out) noexcept {
    LineWriter line{out};
    put_time(line, record.time);
    line.put(' ');
    line.put(std::string_view{to_string(record.level)});
    line.put(' ');
    put_message(line, record);
    return line.finish();
}

void write_log_line_to_fd(const char* const text, const std::size_t length, void* const context) noexcept {
    COMMON_ASSERT(context != nullptr);
    COMMON_ASSERT((text != nullptr) || (length == 0U));
    // The guard drops the line when assertions are disabled.
    if ((context != nullptr) && (text != nullptr)) {
        const NativeHandle descriptor{*static_cast<const NativeHandle*>(context)};
        const Span<const char> line{text, length};
        std::size_t written{0U};
#if defined(_WIN32)
        // Each round writes at least one byte or ends the loop.
        for (std::size_t round{0U}; (written < length) && (round < length); ++round) {
            const Span<const char> rest{line.subspan(written)};
            DWORD count{0U};
            if ((::WriteFile(descriptor, rest.data(), detail::transfer_count(rest.size()), &count, nullptr) != FALSE) &&
                (count > 0U)) {
                written += count;
            } else {
                break;  // A sink cannot report errors: the rest of the line is dropped.
            }
        }
#else
        std::uint32_t restarts{0U};
        // Each round writes at least one byte, or restarts after EINTR, or ends the loop.
        for (std::size_t round{0U}; (written < length) && (round < (length + kMaxWriteRestarts)); ++round) {
            const Span<const char> rest{line.subspan(written)};
            const ssize_t count{::write(descriptor, rest.data(), rest.size())};
            if (count > 0) {
                written += static_cast<std::size_t>(count);
            } else if ((count < 0) && (errno == EINTR) && (restarts < kMaxWriteRestarts)) {
                ++restarts;
            } else {
                break;  // A sink cannot report errors: the rest of the line is dropped.
            }
        }
#endif
    }
}

}  // namespace common
