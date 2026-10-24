/// @file static_string.hpp
/// @brief Fixed-capacity, NUL-terminated character string with inline storage.

#ifndef COMMON_CONTAINERS_STATIC_STRING_HPP
#define COMMON_CONTAINERS_STATIC_STRING_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "common/containers/span.hpp"
#include "common/core/assert.hpp"

namespace common {

/// std::string-like text buffer whose capacity is fixed at compile time, e.g. for building a
/// command for a device or a path. It never allocates: an append that does not fit fails as a
/// whole and leaves the string unchanged, so a string never holds a truncated command.
///
/// The characters are always followed by a NUL, so c_str() can be passed to C interfaces. The
/// string itself may contain NUL characters; size() counts them.
///
/// Numbers are appended with the formatting functions of protocol/text.hpp, e.g. append_hex().
///
/// Not thread-safe.
template <std::size_t Capacity>
class StaticString final {
    static_assert(Capacity > 0U, "Capacity must be positive");
    // bytes() views the characters as bytes, which is valid because std::uint8_t is unsigned char.
    static_assert(std::is_same<std::uint8_t, unsigned char>::value, "std::uint8_t must be unsigned char");

public:
    /// Empty string.
    constexpr StaticString() noexcept = default;

    /// Appends @p character. @return false (the string is unchanged) if the string is full.
    [[nodiscard]] constexpr bool append(const char character) noexcept {
        const bool fits{size_ < Capacity};
        if (fits) {
            storage_[size_] = character;
            ++size_;
            storage_[size_] = '\0';
        }
        return fits;
    }

    /// Appends all of @p text. @return false (the string is unchanged) if it does not fit.
    [[nodiscard]] constexpr bool append(const Span<const char> text) noexcept {
        const bool fits{text.size() <= (Capacity - size_)};
        if (fits) {
            for (const char character : text) {
                storage_[size_] = character;
                ++size_;
            }
            storage_[size_] = '\0';
        }
        return fits;
    }

    /// Appends all of @p text, e.g. a string literal. @return false (the string is unchanged) if
    /// it does not fit.
    [[nodiscard]] constexpr bool append(const std::string_view text) noexcept {
        return append(Span<const char>{text.data(), text.size()});
    }

    /// Replaces the contents with @p text. @return false (the string is unchanged) if it does not fit.
    [[nodiscard]] constexpr bool assign(const std::string_view text) noexcept {
        const bool fits{text.size() <= Capacity};
        if (fits) {
            clear();
            static_cast<void>(append(text));
        }
        return fits;
    }

    /// Shortens the string to its first @p count characters. @pre count <= size()
    constexpr void truncate(const std::size_t count) noexcept {
        COMMON_ASSERT(count <= size_);
        size_ = count;
        storage_[size_] = '\0';
    }

    constexpr void clear() noexcept {
        truncate(0U);
    }

    /// @pre index < size()
    [[nodiscard]] constexpr char operator[](const std::size_t index) const noexcept {
        COMMON_ASSERT(index < size_);
        return storage_[index];
    }

    /// The characters, without the terminating NUL. Valid until the string is modified.
    [[nodiscard]] constexpr Span<const char> text() const noexcept {
        return Span<const char>{storage_.data(), size_};
    }

    /// The characters as a std::string_view. Valid until the string is modified.
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view{storage_.data(), size_};
    }

    /// The characters as bytes, e.g. to write them to a device. Valid until the string is modified.
    [[nodiscard]] Span<const std::uint8_t> bytes() const noexcept {
        // A pointer to unsigned char may access the bytes of any object (exception to MISRA C++:2023
        // Rule 8.2.5).
        return Span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(storage_.data()), size_};
    }

    /// The characters followed by a NUL. Valid until the string is modified.
    [[nodiscard]] constexpr const char* c_str() const noexcept {
        return storage_.data();
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0U;
    }

    [[nodiscard]] constexpr bool full() const noexcept {
        return size_ == Capacity;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    std::array<char, Capacity + 1U> storage_{};  ///< The characters and a terminating NUL.
    std::size_t size_{0U};
};

}  // namespace common

#endif  // COMMON_CONTAINERS_STATIC_STRING_HPP
