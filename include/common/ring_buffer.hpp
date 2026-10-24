/// @file ring_buffer.hpp
/// @brief Fixed-capacity circular buffer (single-threaded), e.g. for sliding windows.

#ifndef COMMON_RING_BUFFER_HPP
#define COMMON_RING_BUFFER_HPP

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "common/assert.hpp"

namespace common {

/// Circular buffer holding the last @p Capacity elements. Index 0 is the oldest element.
/// Not thread-safe; use SpscQueue to move data between threads.
template <typename T, std::size_t Capacity>
class RingBuffer final {
    static_assert(Capacity > 0U, "Capacity must be positive");
    static_assert(std::is_nothrow_default_constructible<T>::value, "T must be nothrow default-constructible");
    static_assert(std::is_nothrow_copy_assignable<T>::value, "T must be nothrow copy-assignable");

public:
    /// Appends @p value, overwriting the oldest element when full.
    /// @return true if an element was overwritten.
    bool push_overwrite(const T& value) noexcept {
        const bool overwrite{full()};
        buffer_[physical(size_ < Capacity ? size_ : 0U)] = value;
        if (overwrite) {
            head_ = advance(head_);
        } else {
            ++size_;
        }
        return overwrite;
    }

    /// Appends @p value. @return false if the buffer is full (nothing is overwritten).
    [[nodiscard]] bool try_push(const T& value) noexcept {
        bool pushed{false};
        if (!full()) {
            buffer_[physical(size_)] = value;
            ++size_;
            pushed = true;
        }
        return pushed;
    }

    /// Removes the oldest element. @return false if empty.
    [[nodiscard]] bool try_pop_front(T& out) noexcept {
        bool popped{false};
        if (!empty()) {
            out = std::move(buffer_[head_]);
            head_ = advance(head_);
            --size_;
            popped = true;
        }
        return popped;
    }

    /// @pre index < size(); index 0 is the oldest element.
    [[nodiscard]] const T& operator[](const std::size_t index) const noexcept {
        COMMON_ASSERT(index < size_);
        return buffer_[physical(index)];
    }

    /// @pre !empty()
    [[nodiscard]] const T& front() const noexcept {
        return (*this)[0U];
    }

    /// @pre !empty()
    [[nodiscard]] const T& back() const noexcept {
        return (*this)[size_ - 1U];
    }

    void clear() noexcept {
        head_ = 0U;
        size_ = 0U;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0U;
    }
    [[nodiscard]] bool full() const noexcept {
        return size_ == Capacity;
    }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    /// Maps a logical index (< Capacity) to a buffer position without a division.
    std::size_t physical(const std::size_t logical) const noexcept {
        const std::size_t position{head_ + logical};
        return (position >= Capacity) ? (position - Capacity) : position;
    }

    static std::size_t advance(const std::size_t position) noexcept {
        return ((position + 1U) == Capacity) ? 0U : (position + 1U);
    }

    std::array<T, Capacity> buffer_{};
    std::size_t head_{0U};
    std::size_t size_{0U};
};

}  // namespace common

#endif  // COMMON_RING_BUFFER_HPP
