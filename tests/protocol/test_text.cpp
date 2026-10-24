#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/core/status.hpp"
#include "common/protocol/text.hpp"
#include "test_framework.hpp"

namespace {

common::Span<const char> span_of(const std::string_view text) {
    return common::Span<const char>{text.data(), text.size()};
}

std::string_view view_of(const common::Span<const char> text) {
    return std::string_view{text.data(), text.size()};
}

}  // namespace

TEST_CASE(text_classification_and_trim) {
    CHECK(common::is_space(' '));
    CHECK(common::is_space('\t'));
    CHECK(common::is_space('\r'));
    CHECK(common::is_space('\n'));
    CHECK(!common::is_space('a'));
    CHECK(!common::is_space('\0'));
    CHECK(common::is_decimal_digit('0'));
    CHECK(!common::is_decimal_digit('a'));
    CHECK(common::is_hex_digit('f'));
    CHECK(common::is_hex_digit('F'));
    CHECK(!common::is_hex_digit('g'));

    CHECK(common::hex_digit_value('7').value() == 7U);
    CHECK(common::hex_digit_value('a').value() == 10U);
    CHECK(common::hex_digit_value('F').value() == 15U);
    CHECK(common::hex_digit_value('G').status() == common::Status::kInvalidArgument);

    CHECK(view_of(common::trim(span_of("  41 0C \r\n"))) == "41 0C");
    CHECK(view_of(common::trim(span_of("OK"))) == "OK");
    CHECK(common::trim(span_of(" \r\n\t")).empty());
    CHECK(common::trim(common::Span<const char>{}).empty());
}

TEST_CASE(text_classification_of_every_character) {
    // The expected values come from the ASCII codes; the library itself never uses them.
    std::uint32_t mismatches{0U};
    for (std::uint32_t code{0U}; code < 256U; ++code) {
        const char character{static_cast<char>(code)};
        const bool digit{(code >= 0x30U) && (code <= 0x39U)};
        const bool upper{(code >= 0x41U) && (code <= 0x46U)};
        const bool lower{(code >= 0x61U) && (code <= 0x66U)};
        const bool space{(code == 0x20U) || ((code >= 0x09U) && (code <= 0x0DU))};
        mismatches += (common::is_decimal_digit(character) == digit) ? 0U : 1U;
        mismatches += (common::is_hex_digit(character) == (digit || upper || lower)) ? 0U : 1U;
        mismatches += (common::is_space(character) == space) ? 0U : 1U;

        std::uint32_t expected{99U};
        if (digit) {
            expected = code - 0x30U;
        } else if (upper) {
            expected = (code - 0x41U) + 10U;
        } else if (lower) {
            expected = (code - 0x61U) + 10U;
        } else {
            // Not a hexadecimal digit.
        }
        const common::Result<std::uint8_t> value{common::hex_digit_value(character)};
        mismatches += (value.has_value() == (expected != 99U)) ? 0U : 1U;
        mismatches += (std::uint32_t{value.value_or(99U)} == expected) ? 0U : 1U;

        const common::Result<std::uint8_t> parsed{
            common::parse_unsigned<std::uint8_t>(common::Span<const char>{&character, 1U})};
        mismatches += (parsed.has_value() == digit) ? 0U : 1U;
        mismatches += (!digit || (std::uint32_t{parsed.value_or(99U)} == (code - 0x30U))) ? 0U : 1U;
    }
    CHECK(mismatches == 0U);

    // Formatting through the digit tables matches std::to_string.
    std::array<char, 24U> buffer{};
    for (std::uint32_t value{0U}; value < 2000U; ++value) {
        const common::Result<std::size_t> length{common::format_unsigned(value * 7919U, common::Span<char>{buffer})};
        CHECK(view_of(common::Span<char>{buffer}.first(length.value_or(0U))) == std::to_string(value * 7919U));
    }
}

TEST_CASE(text_matching) {
    const common::Span<const char> line{span_of("SEARCHING...")};
    CHECK(common::equals(line, "SEARCHING..."));
    CHECK(!common::equals(line, "SEARCHING"));
    CHECK(!common::equals(line, "searching..."));
    CHECK(common::starts_with(line, "SEARCH"));
    CHECK(common::starts_with(line, ""));
    CHECK(!common::starts_with(line, "SEARCHING...!"));
    CHECK(common::ends_with(line, "..."));
    CHECK(!common::ends_with(line, "SEARCHING"));
    CHECK(common::contains(span_of("BUS INIT: ...ERROR"), "ERROR"));
    CHECK(!common::contains(span_of("NO DATA"), "ERROR"));
    CHECK(common::find(span_of("ELM327 v1.5"), "v").value() == 7U);
    CHECK(common::find(span_of("abc"), "").value() == 0U);
    CHECK(common::find(span_of("ab"), "abc").status() == common::Status::kNotFound);
    CHECK(common::equals(common::Span<const char>{}, ""));

    static_assert(common::starts_with(common::Span<const char>{"41 0C", 5U}, "41"), "constexpr");
}

TEST_CASE(text_parse_numbers) {
    CHECK(common::parse_unsigned<std::uint32_t>(span_of("1200")).value() == 1200U);
    CHECK(common::parse_unsigned<std::uint32_t>(span_of("007")).value() == 7U);
    CHECK(common::parse_unsigned<std::uint8_t>(span_of("255")).value() == 255U);
    CHECK(common::parse_unsigned<std::uint8_t>(span_of("256")).status() == common::Status::kOverflow);
    CHECK(common::parse_unsigned<std::uint64_t>(span_of("18446744073709551615")).value() == UINT64_MAX);
    CHECK(common::parse_unsigned<std::uint64_t>(span_of("18446744073709551616")).status() ==
          common::Status::kOverflow);
    CHECK(common::parse_unsigned<std::uint32_t>(span_of("")).status() == common::Status::kInvalidArgument);
    CHECK(common::parse_unsigned<std::uint32_t>(span_of("-1")).status() == common::Status::kInvalidArgument);
    CHECK(common::parse_unsigned<std::uint32_t>(span_of("12 ")).status() == common::Status::kInvalidArgument);

    CHECK(common::parse_hex<std::uint16_t>(span_of("7E8")).value() == 0x7E8U);
    CHECK(common::parse_hex<std::uint16_t>(span_of("1af8")).value() == 0x1AF8U);
    CHECK(common::parse_hex<std::uint8_t>(span_of("00FF")).value() == 0xFFU);  // Leading zeros fit.
    CHECK(common::parse_hex<std::uint8_t>(span_of("100")).status() == common::Status::kOverflow);
    CHECK(common::parse_hex<std::uint32_t>(span_of("18DAF110")).value() == 0x18DAF110U);
    CHECK(common::parse_hex<std::uint64_t>(span_of("FFFFFFFFFFFFFFFF")).value() == UINT64_MAX);
    CHECK(common::parse_hex<std::uint64_t>(span_of("10000000000000000")).status() == common::Status::kOverflow);
    CHECK(common::parse_hex<std::uint16_t>(span_of("0x10")).status() == common::Status::kInvalidArgument);
    CHECK(common::parse_hex<std::uint16_t>(span_of("")).status() == common::Status::kInvalidArgument);
}

TEST_CASE(text_decode_hex_bytes) {
    std::array<std::uint8_t, 4U> out{};
    const common::Result<std::size_t> spaced{common::decode_hex_bytes(span_of("41 0C 1A F8"), out)};
    CHECK(spaced.value() == 4U);
    CHECK((out == std::array<std::uint8_t, 4U>{{0x41U, 0x0CU, 0x1AU, 0xF8U}}));

    out = {};
    CHECK(common::decode_hex_bytes(span_of("410c1af8"), out).value() == 4U);
    CHECK(out[3U] == 0xF8U);
    CHECK(common::decode_hex_bytes(span_of("  7E8 \r"), out).status() == common::Status::kInvalidArgument);
    CHECK(common::decode_hex_bytes(span_of(" 41  0C \r"), out).value() == 2U);
    CHECK(common::decode_hex_bytes(span_of(""), out).value() == 0U);
    CHECK(common::decode_hex_bytes(span_of("   "), out).value() == 0U);
    CHECK(common::decode_hex_bytes(span_of("4 10C"), out).status() == common::Status::kInvalidArgument);
    CHECK(common::decode_hex_bytes(span_of("NO DATA"), out).status() == common::Status::kInvalidArgument);
    CHECK(common::decode_hex_bytes(span_of("01 02 03 04 05"), out).status() == common::Status::kOutOfRange);
    CHECK(out[3U] == 0x04U);  // The bytes that fit were written.
}

TEST_CASE(text_format_numbers) {
    std::array<char, 24U> buffer{};
    const common::Span<char> out{buffer};

    common::Result<std::size_t> length{common::format_unsigned(std::uint32_t{1200U}, out)};
    CHECK(view_of(out.first(length.value())) == "1200");
    length = common::format_unsigned(std::uint8_t{0U}, out);
    CHECK(view_of(out.first(length.value())) == "0");
    length = common::format_unsigned(UINT64_MAX, out);
    CHECK(view_of(out.first(length.value())) == "18446744073709551615");
    CHECK(common::format_unsigned(std::uint32_t{1200U}, out.first(3U)).status() == common::Status::kOutOfRange);

    length = common::format_hex(std::uint16_t{0x7E0U}, 3U, out);
    CHECK(view_of(out.first(length.value())) == "7E0");
    length = common::format_hex(std::uint8_t{0x0CU}, 2U, out);
    CHECK(view_of(out.first(length.value())) == "0C");
    length = common::format_hex(UINT64_MAX, 16U, out);
    CHECK(view_of(out.first(length.value())) == "FFFFFFFFFFFFFFFF");
    CHECK(common::format_hex(std::uint16_t{0x7E0U}, 2U, out).status() == common::Status::kOutOfRange);
    CHECK(common::format_hex(std::uint16_t{0x7E0U}, 5U, out).status() == common::Status::kInvalidArgument);
    CHECK(common::format_hex(std::uint16_t{0x7E0U}, 0U, out).status() == common::Status::kInvalidArgument);
    CHECK(common::format_hex(std::uint16_t{0x7E0U}, 3U, out.first(2U)).status() == common::Status::kOutOfRange);

    const std::array<std::uint8_t, 3U> bytes{{0x01U, 0x0CU, 0xABU}};
    length = common::format_hex_bytes(bytes, out, ' ');
    CHECK(view_of(out.first(length.value())) == "01 0C AB");
    length = common::format_hex_bytes(bytes, out, '\0');
    CHECK(view_of(out.first(length.value())) == "010CAB");
    CHECK(common::format_hex_bytes(bytes, out.first(7U), ' ').status() == common::Status::kOutOfRange);
    CHECK(common::format_hex_bytes(bytes, out.first(8U), ' ').value() == 8U);
    CHECK(common::format_hex_bytes(common::Span<const std::uint8_t>{}, out.first(0U), ' ').value() == 0U);
}

TEST_CASE(text_append_to_static_string) {
    // Building a device command: "AT SH 7E0\r".
    common::StaticString<16U> command{};
    CHECK(command.append("AT SH "));
    CHECK(common::append_hex(command, std::uint16_t{0x7E0U}, 3U));
    CHECK(command.append('\r'));
    CHECK(command.view() == "AT SH 7E0\r");

    common::StaticString<16U> request{};
    const std::array<std::uint8_t, 2U> pid{{0x01U, 0x0CU}};
    CHECK(common::append_hex_bytes(request, pid, ' '));
    CHECK(common::append_unsigned(request, std::uint8_t{1U}));
    CHECK(request.view() == "01 0C1");

    // All or nothing.
    common::StaticString<4U> small{};
    CHECK(small.append("AB"));
    CHECK(!common::append_hex_bytes(small, pid, ' '));
    CHECK(small.view() == "AB");
    CHECK(!common::append_unsigned(small, std::uint32_t{12345U}));
    CHECK(!common::append_hex(small, std::uint16_t{0x7E0U}, 3U));
    CHECK(!common::append_hex(small, std::uint16_t{0x7E0U}, 1U));
    CHECK(small.view() == "AB");
}
