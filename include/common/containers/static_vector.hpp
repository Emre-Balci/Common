/// @file static_vector.hpp
/// @brief Fixed-capacity vector with inline storage.

#ifndef COMMON_CONTAINERS_STATIC_VECTOR_HPP
#define COMMON_CONTAINERS_STATIC_VECTOR_HPP

#include <array>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "common/core/assert.hpp"

namespace common {

/// std::vector-like container whose capacity is fixed at compile time. It never allocates;
/// insertion into a full vector fails and reports it instead of growing.
///
/// Not thread-safe.
template <typename T, std::size_t Capacity>
class StaticVector final {
    static_assert(Capacity > 0U, "Capacity must be positive");
    static_assert(std::is_nothrow_destructible<T>::value, "T must be nothrow destructible");

public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    StaticVector() noexcept = default;

    StaticVector(const StaticVector& other) noexcept {
        copy_from(other);
    }

    StaticVector(StaticVector&& other) noexcept {
        move_from(other);
    }

    StaticVector& operator=(const StaticVector& other) noexcept {
        if (this != &other) {
            clear();
            copy_from(other);
        }
        return *this;
    }

    StaticVector& operator=(StaticVector&& other) noexcept {
        if (this != &other) {
            clear();
            move_from(other);
        }
        return *this;
    }

    ~StaticVector() {
        clear();
    }

    /// @return false if the vector is full (nothing is constructed).
    template <typename... Args>
    [[nodiscard]] bool emplace_back(Args&&... args) noexcept {
        static_assert(std::is_nothrow_constructible<T, Args...>::value, "construction must be noexcept");
        bool inserted{false};
        if (size_ < Capacity) {
            // Deviation (MISRA C++:2023 Rule 21.6.3): placement new into owned, suitably aligned storage.
            static_cast<void>(::new (slot(size_)) T(std::forward<Args>(args)...));
            ++size_;
            inserted = true;
        }
        return inserted;
    }

    [[nodiscard]] bool push_back(const T& value) noexcept {
        return emplace_back(value);
    }

    [[nodiscard]] bool push_back(T&& value) noexcept {
        return emplace_back(std::move(value));
    }

    /// @pre !empty()
    void pop_back() noexcept {
        COMMON_ASSERT(size_ > 0U);
        --size_;
        element(size_)->~T();
    }

    /// Removes the element at @p index by moving the last element into its place. O(1).
    /// @pre index < size()
    void erase_unordered(const std::size_t index) noexcept {
        COMMON_ASSERT(index < size_);
        const std::size_t last{size_ - 1U};
        if (index != last) {
            *element(index) = std::move(*element(last));
        }
        pop_back();
    }

    /// Removes the element at @p index preserving order. O(size()).
    /// @pre index < size()
    void erase(const std::size_t index) noexcept {
        COMMON_ASSERT(index < size_);
        for (std::size_t i{index + 1U}; i < size_; ++i) {
            *element(i - 1U) = std::move(*element(i));
        }
        pop_back();
    }

    void clear() noexcept {
        while (size_ > 0U) {
            pop_back();
        }
    }

    /// @pre index < size()
    [[nodiscard]] T& operator[](const std::size_t index) noexcept {
        COMMON_ASSERT(index < size_);
        return *element(index);
    }

    /// @pre index < size()
    [[nodiscard]] const T& operator[](const std::size_t index) const noexcept {
        COMMON_ASSERT(index < size_);
        return *element(index);
    }

    [[nodiscard]] T& front() noexcept {
        return (*this)[0U];
    }
    [[nodiscard]] const T& front() const noexcept {
        return (*this)[0U];
    }
    [[nodiscard]] T& back() noexcept {
        return (*this)[size_ - 1U];
    }
    [[nodiscard]] const T& back() const noexcept {
        return (*this)[size_ - 1U];
    }

    [[nodiscard]] T* data() noexcept {
        return element(0U);
    }
    [[nodiscard]] const T* data() const noexcept {
        return element(0U);
    }

    [[nodiscard]] iterator begin() noexcept {
        return data();
    }
    [[nodiscard]] iterator end() noexcept {
        return data() + size_;
    }
    [[nodiscard]] const_iterator begin() const noexcept {
        return data();
    }
    [[nodiscard]] const_iterator end() const noexcept {
        return data() + size_;
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
    void* slot(const std::size_t index) noexcept {
        return &storage_[index * sizeof(T)];
    }

    // Deviation (MISRA C++:2023 Rule 8.2.5): reinterpret_cast from raw storage to the object
    // created in it by placement new; std::launder makes the access well-defined.
    T* element(const std::size_t index) noexcept {
        return std::launder(reinterpret_cast<T*>(&storage_[index * sizeof(T)]));
    }
    const T* element(const std::size_t index) const noexcept {
        return std::launder(reinterpret_cast<const T*>(&storage_[index * sizeof(T)]));
    }

    void copy_from(const StaticVector& other) noexcept {
        static_assert(std::is_nothrow_copy_constructible<T>::value, "T must be nothrow copy-constructible");
        for (std::size_t i{0U}; i < other.size_; ++i) {
            static_cast<void>(::new (slot(i)) T(*other.element(i)));
            ++size_;
        }
    }

    void move_from(StaticVector& other) noexcept {
        static_assert(std::is_nothrow_move_constructible<T>::value, "T must be nothrow move-constructible");
        for (std::size_t i{0U}; i < other.size_; ++i) {
            static_cast<void>(::new (slot(i)) T(std::move(*other.element(i))));
            ++size_;
        }
        other.clear();
    }

    alignas(T) std::array<std::byte, sizeof(T) * Capacity> storage_;
    std::size_t size_{0U};
};

}  // namespace common

#endif  // COMMON_CONTAINERS_STATIC_VECTOR_HPP
