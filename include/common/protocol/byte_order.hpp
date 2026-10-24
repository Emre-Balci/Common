/// @file byte_order.hpp
/// @brief Bounds-checked reading and writing of multi-byte integers and bits in message buffers.
///
/// Protocol fields have a defined byte order that is independent of the CPU: big-endian (most
/// significant byte first; e.g. OBD-II and most network protocols) or little-endian (least
/// significant byte first). These functions assemble and split the bytes with shifts, so they work
/// on any CPU and need no alignment, unlike casting a buffer pointer to an integer pointer.
///
/// Offsets and lengths usually come from received data, so every function checks them and reports
/// kOutOfRange instead of asserting: a short or malformed message is an expected runtime error.
///
/// Every function is noexcept, never allocates or blocks, and loops at most over sizeof(T) or 8
/// bytes, so all of them are safe on the real-time path.

#ifndef COMMON_PROTOCOL_BYTE_ORDER_HPP
#define COMMON_PROTOCOL_BYTE_ORDER_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"

namespace common {

namespace detail {
namespace byte_order {

template <typename T>
inline constexpr bool kIsUnsignedInteger{std::is_integral<T>::value && std::is_unsigned<T>::value &&
                                         (!std::is_same<T, bool>::value)};

inline constexpr std::uint32_t kBitsPerByte{8U};

/// Assembles @p count bytes from @p offset, most significant first. @pre bytes.in_bounds(offset, count), count <= 8
[[nodiscard]] constexpr std::uint64_t assemble_big_endian(const Span<const std::uint8_t> bytes, const std::size_t offset,
                                                          const std::size_t count) noexcept {
    std::uint64_t value{0U};
    for (std::size_t index{0U}; index < count; ++index) {
        value = (value << kBitsPerByte) | bytes[offset + index];
    }
    return value;
}

/// Assembles @p count bytes from @p offset, least significant first. @pre bytes.in_bounds(offset, count), count <= 8
[[nodiscard]] constexpr std::uint64_t assemble_little_endian(const Span<const std::uint8_t> bytes,
                                                             const std::size_t offset, const std::size_t count) noexcept {
    std::uint64_t value{0U};
    for (std::size_t index{0U}; index < count; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * kBitsPerByte);
    }
    return value;
}

}  // namespace byte_order
}  // namespace detail

/// Reads a T stored big-endian at @p offset, e.g. read_big_endian<std::uint16_t>(data, 2U) gives
/// data[2] * 256 + data[3]. @return kOutOfRange if the bytes are not all inside @p bytes.
template <typename T>
[[nodiscard]] constexpr Result<T> read_big_endian(const Span<const std::uint8_t> bytes, const std::size_t offset) noexcept {
    static_assert(detail::byte_order::kIsUnsignedInteger<T>, "T must be an unsigned integer type");
    return bytes.in_bounds(offset, sizeof(T))
               ? Result<T>::success(static_cast<T>(detail::byte_order::assemble_big_endian(bytes, offset, sizeof(T))))
               : Result<T>::failure(Status::kOutOfRange);
}

/// Reads a T stored little-endian at @p offset. @return kOutOfRange if the bytes are not all inside @p bytes.
template <typename T>
[[nodiscard]] constexpr Result<T> read_little_endian(const Span<const std::uint8_t> bytes,
                                                     const std::size_t offset) noexcept {
    static_assert(detail::byte_order::kIsUnsignedInteger<T>, "T must be an unsigned integer type");
    return bytes.in_bounds(offset, sizeof(T))
               ? Result<T>::success(static_cast<T>(detail::byte_order::assemble_little_endian(bytes, offset, sizeof(T))))
               : Result<T>::failure(Status::kOutOfRange);
}

/// Reads an unsigned field of @p count bytes (1 to 8) stored big-endian at @p offset, for widths
/// without an integer type, e.g. a 3-byte counter.
/// @return kInvalidArgument if @p count is 0 or above 8; kOutOfRange if the bytes are not all inside @p bytes.
[[nodiscard]] constexpr Result<std::uint64_t> read_big_endian_field(const Span<const std::uint8_t> bytes,
                                                                    const std::size_t offset,
                                                                    const std::size_t count) noexcept {
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kInvalidArgument)};
    if ((count > 0U) && (count <= sizeof(std::uint64_t))) {
        result = bytes.in_bounds(offset, count)
                     ? Result<std::uint64_t>::success(detail::byte_order::assemble_big_endian(bytes, offset, count))
                     : Result<std::uint64_t>::failure(Status::kOutOfRange);
    }
    return result;
}

/// Writes @p value big-endian at @p offset. @return kOutOfRange (nothing is written) if the bytes
/// are not all inside @p bytes.
template <typename T>
[[nodiscard]] constexpr Status write_big_endian(const T value, const Span<std::uint8_t> bytes,
                                                const std::size_t offset) noexcept {
    static_assert(detail::byte_order::kIsUnsignedInteger<T>, "T must be an unsigned integer type");
    const bool fits{bytes.in_bounds(offset, sizeof(T))};
    if (fits) {
        const std::uint64_t wide{value};
        for (std::size_t index{0U}; index < sizeof(T); ++index) {
            const std::size_t shift{(sizeof(T) - 1U - index) * detail::byte_order::kBitsPerByte};
            bytes[offset + index] = static_cast<std::uint8_t>(wide >> shift);
        }
    }
    return fits ? Status::kOk : Status::kOutOfRange;
}

/// Writes @p value little-endian at @p offset. @return kOutOfRange (nothing is written) if the
/// bytes are not all inside @p bytes.
template <typename T>
[[nodiscard]] constexpr Status write_little_endian(const T value, const Span<std::uint8_t> bytes,
                                                   const std::size_t offset) noexcept {
    static_assert(detail::byte_order::kIsUnsignedInteger<T>, "T must be an unsigned integer type");
    const bool fits{bytes.in_bounds(offset, sizeof(T))};
    if (fits) {
        const std::uint64_t wide{value};
        for (std::size_t index{0U}; index < sizeof(T); ++index) {
            bytes[offset + index] = static_cast<std::uint8_t>(wide >> (index * detail::byte_order::kBitsPerByte));
        }
    }
    return fits ? Status::kOk : Status::kOutOfRange;
}

/// Tests bit @p bit of @p bytes, numbering the bits from the most significant bit of bytes[0]
/// (bit 0) to the least significant bit of the last byte. This is the order of bitmaps that list
/// supported items, such as the OBD-II "supported PIDs" responses.
/// @return kOutOfRange if the bit lies beyond the last byte.
[[nodiscard]] constexpr Result<bool> test_bit_msb_first(const Span<const std::uint8_t> bytes,
                                                        const std::size_t bit) noexcept {
    const std::size_t byte_index{bit / detail::byte_order::kBitsPerByte};
    const std::size_t bit_in_byte{bit % detail::byte_order::kBitsPerByte};
    return (byte_index < bytes.size())
               ? Result<bool>::success(((static_cast<std::uint32_t>(bytes[byte_index]) >> (7U - bit_in_byte)) & 1U) != 0U)
               : Result<bool>::failure(Status::kOutOfRange);
}

/// Tests bit @p bit of @p bytes, numbering the bits from the least significant bit of bytes[0]
/// (bit 0) to the most significant bit of the last byte.
/// @return kOutOfRange if the bit lies beyond the last byte.
[[nodiscard]] constexpr Result<bool> test_bit_lsb_first(const Span<const std::uint8_t> bytes,
                                                        const std::size_t bit) noexcept {
    const std::size_t byte_index{bit / detail::byte_order::kBitsPerByte};
    const std::size_t bit_in_byte{bit % detail::byte_order::kBitsPerByte};
    return (byte_index < bytes.size())
               ? Result<bool>::success(((static_cast<std::uint32_t>(bytes[byte_index]) >> bit_in_byte) & 1U) != 0U)
               : Result<bool>::failure(Status::kOutOfRange);
}

}  // namespace common

#endif  // COMMON_PROTOCOL_BYTE_ORDER_HPP
