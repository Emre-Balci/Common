#include <array>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/protocol/byte_order.hpp"
#include "test_framework.hpp"

TEST_CASE(byte_order_read) {
    // An OBD-II response to "01 0C" (engine speed): 41 0C 1A F8.
    const std::array<std::uint8_t, 4U> response{{0x41U, 0x0CU, 0x1AU, 0xF8U}};
    const common::Span<const std::uint8_t> bytes{response};
    CHECK(common::read_big_endian<std::uint16_t>(bytes, 2U).value() == 0x1AF8U);
    CHECK(common::read_little_endian<std::uint16_t>(bytes, 2U).value() == 0xF81AU);
    CHECK(common::read_big_endian<std::uint8_t>(bytes, 0U).value() == 0x41U);
    CHECK(common::read_big_endian<std::uint32_t>(bytes, 0U).value() == 0x410C1AF8U);
    CHECK(common::read_little_endian<std::uint32_t>(bytes, 0U).value() == 0xF81A0C41U);
    CHECK(common::read_big_endian<std::uint16_t>(bytes, 3U).status() == common::Status::kOutOfRange);
    CHECK(common::read_big_endian<std::uint64_t>(bytes, 0U).status() == common::Status::kOutOfRange);
    CHECK(common::read_little_endian<std::uint16_t>(bytes, 99U).status() == common::Status::kOutOfRange);

    CHECK(common::read_big_endian_field(bytes, 1U, 3U).value() == 0x0C1AF8U);
    CHECK(common::read_big_endian_field(bytes, 0U, 4U).value() == 0x410C1AF8U);
    CHECK(common::read_big_endian_field(bytes, 2U, 3U).status() == common::Status::kOutOfRange);
    CHECK(common::read_big_endian_field(bytes, 0U, 0U).status() == common::Status::kInvalidArgument);
    CHECK(common::read_big_endian_field(bytes, 0U, 9U).status() == common::Status::kInvalidArgument);

    const std::array<std::uint8_t, 8U> all{{0xFFU, 0xFEU, 0xFDU, 0xFCU, 0xFBU, 0xFAU, 0xF9U, 0xF8U}};
    CHECK(common::read_big_endian<std::uint64_t>(all, 0U).value() == 0xFFFEFDFCFBFAF9F8U);
    CHECK(common::read_little_endian<std::uint64_t>(all, 0U).value() == 0xF8F9FAFBFCFDFEFFU);
    CHECK(common::read_big_endian_field(all, 0U, 8U).value() == 0xFFFEFDFCFBFAF9F8U);

    static_assert(common::read_big_endian<std::uint16_t>(
                      common::Span<const std::uint8_t>{std::array<std::uint8_t, 2U>{{1U, 2U}}.data(), 2U}, 0U)
                          .status() == common::Status::kOk,
                  "constexpr");
}

TEST_CASE(byte_order_write) {
    std::array<std::uint8_t, 6U> buffer{};
    CHECK(common::is_ok(common::write_big_endian(std::uint16_t{0x1AF8U}, buffer, 0U)));
    CHECK(common::is_ok(common::write_little_endian(std::uint32_t{0x01020304U}, buffer, 2U)));
    CHECK((buffer == std::array<std::uint8_t, 6U>{{0x1AU, 0xF8U, 0x04U, 0x03U, 0x02U, 0x01U}}));

    // Out of range: nothing is written.
    CHECK(common::write_big_endian(std::uint32_t{0xAABBCCDDU}, buffer, 3U) == common::Status::kOutOfRange);
    CHECK(common::write_little_endian(std::uint16_t{0xAABBU}, buffer, 5U) == common::Status::kOutOfRange);
    CHECK((buffer == std::array<std::uint8_t, 6U>{{0x1AU, 0xF8U, 0x04U, 0x03U, 0x02U, 0x01U}}));

    // Round trip.
    std::array<std::uint8_t, 8U> wide{};
    CHECK(common::is_ok(common::write_big_endian(std::uint64_t{0x0123456789ABCDEFU}, wide, 0U)));
    CHECK(common::read_big_endian<std::uint64_t>(wide, 0U).value() == 0x0123456789ABCDEFU);
    CHECK(common::is_ok(common::write_little_endian(std::uint64_t{0x0123456789ABCDEFU}, wide, 0U)));
    CHECK(common::read_little_endian<std::uint64_t>(wide, 0U).value() == 0x0123456789ABCDEFU);
}

TEST_CASE(byte_order_bits) {
    // OBD-II "01 00" bitmap BE 3E B8 11: bit 0 (PID 01) is the MSB of 0xBE.
    const std::array<std::uint8_t, 4U> bitmap{{0xBEU, 0x3EU, 0xB8U, 0x11U}};
    CHECK(common::test_bit_msb_first(bitmap, 0U).value());   // PID 01
    CHECK(!common::test_bit_msb_first(bitmap, 1U).value());  // PID 02
    CHECK(common::test_bit_msb_first(bitmap, 2U).value());   // PID 03
    CHECK(!common::test_bit_msb_first(bitmap, 7U).value());  // PID 08
    CHECK(common::test_bit_msb_first(bitmap, 31U).value());  // PID 20: more PIDs follow.
    CHECK(common::test_bit_msb_first(bitmap, 32U).status() == common::Status::kOutOfRange);

    CHECK(!common::test_bit_lsb_first(bitmap, 0U).value());
    CHECK(common::test_bit_lsb_first(bitmap, 1U).value());
    CHECK(common::test_bit_lsb_first(bitmap, 7U).value());
    CHECK(common::test_bit_lsb_first(bitmap, 24U).value());
    CHECK(!common::test_bit_lsb_first(bitmap, 31U).value());
    CHECK(common::test_bit_lsb_first(bitmap, 32U).status() == common::Status::kOutOfRange);
}
