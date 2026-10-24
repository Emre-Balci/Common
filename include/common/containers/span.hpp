/// @file span.hpp
/// @brief Non-owning, bounds-checked view of contiguous elements (a C++17 stand-in for std::span).

#ifndef COMMON_CONTAINERS_SPAN_HPP
#define COMMON_CONTAINERS_SPAN_HPP

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "common/core/assert.hpp"
#include "common/core/config.hpp"

namespace common {

template <typename T>
class Span;

namespace detail {

template <typename T>
struct IsSpan final : std::false_type {};

template <typename T>
struct IsSpan<Span<T>> final : std::true_type {};

}  // namespace detail

/// View of size() contiguous objects owned by someone else: a std::array, a StaticVector, a
/// message buffer. A span never allocates and never owns; the viewed storage must outlive it.
///
/// Element access and sub-views check their bounds with COMMON_ASSERT, so an out-of-range index
/// is a reported contract violation instead of undefined behaviour. For lengths that come from
/// untrusted input (e.g. a length field in a message), test in_bounds() first: a failed
/// assertion aborts the process.
///
/// Span<const T> is the read-only form; a Span<T> converts to it implicitly.
template <typename T>
class Span final {
public:
    using element_type = T;
    using value_type = std::remove_cv_t<T>;
    using size_type = std::size_t;
    using iterator = T*;

    /// Empty view.
    constexpr Span() noexcept = default;

    /// @pre data != nullptr, or size == 0
    constexpr Span(T* const data, const std::size_t size) noexcept : data_{data}, size_{size} {
        COMMON_ASSERT((data != nullptr) || (size == 0U));
    }

    /// Views every element of a contiguous container with data() and size(): std::array,
    /// StaticVector, or another library container. Temporaries are rejected, so the span cannot
    /// outlive the container by accident.
    // Deviation (MISRA C++:2023 Rule 15.1.3): implicit like std::span, so a container can be passed
    // where a span is expected. The conversion only creates a view; it never copies or narrows.
    template <typename Container,
              typename = std::enable_if_t<
                  (!detail::IsSpan<std::remove_cv_t<Container>>::value) &&
                  std::is_convertible<decltype(std::declval<Container&>().data()), T*>::value>>
    constexpr Span(Container& container) noexcept  // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
        : Span{container.data(), container.size()} {}

    /// Span<U> converts to Span<const U>.
    // Deviation (MISRA C++:2023 Rule 15.1.3): implicit so that Span<T> can be passed where
    // Span<const T> is expected; it only adds const.
    template <typename U,
              typename = std::enable_if_t<(!std::is_const<U>::value) && std::is_same<const U, T>::value>>
    constexpr Span(const Span<U>& other) noexcept  // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
        : data_{other.data()}, size_{other.size()} {}

    /// @pre index < size()
    [[nodiscard]] constexpr T& operator[](const std::size_t index) const noexcept {
        COMMON_ASSERT(index < size_);
        return data_[index];
    }

    /// @pre !empty()
    [[nodiscard]] constexpr T& front() const noexcept {
        COMMON_ASSERT(size_ > 0U);
        return data_[0U];
    }

    /// @pre !empty()
    [[nodiscard]] constexpr T& back() const noexcept {
        COMMON_ASSERT(size_ > 0U);
        return data_[size_ - 1U];
    }

    /// @return true if [offset, offset + count) lies inside the span. Overflow-safe.
    [[nodiscard]] constexpr bool in_bounds(const std::size_t offset, const std::size_t count) const noexcept {
        return (offset <= size_) && (count <= (size_ - offset));
    }

    /// The first @p count elements. @pre count <= size()
    [[nodiscard]] constexpr Span first(const std::size_t count) const noexcept {
        COMMON_ASSERT(count <= size_);
        return Span{data_, count};
    }

    /// The last @p count elements. @pre count <= size()
    [[nodiscard]] constexpr Span last(const std::size_t count) const noexcept {
        COMMON_ASSERT(count <= size_);
        return Span{data_ + (size_ - count), count};
    }

    /// @p count elements starting at @p offset. @pre in_bounds(offset, count)
    [[nodiscard]] constexpr Span subspan(const std::size_t offset, const std::size_t count) const noexcept {
        COMMON_ASSERT(in_bounds(offset, count));
        return Span{data_ + offset, count};
    }

    /// The elements from @p offset to the end. @pre offset <= size()
    [[nodiscard]] constexpr Span subspan(const std::size_t offset) const noexcept {
        COMMON_ASSERT(offset <= size_);
        return Span{data_ + offset, size_ - offset};
    }

    [[nodiscard]] constexpr T* data() const noexcept {
        return data_;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr std::size_t size_bytes() const noexcept {
        return size_ * sizeof(T);
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0U;
    }

    [[nodiscard]] constexpr iterator begin() const noexcept {
        return data_;
    }

    [[nodiscard]] constexpr iterator end() const noexcept {
        return data_ + size_;
    }

private:
    T* data_{nullptr};
    std::size_t size_{0U};
};

template <typename T>
Span(T*, std::size_t) -> Span<T>;

template <typename T, std::size_t N>
Span(std::array<T, N>&) -> Span<T>;

template <typename T, std::size_t N>
Span(const std::array<T, N>&) -> Span<const T>;

}  // namespace common

#endif  // COMMON_CONTAINERS_SPAN_HPP
