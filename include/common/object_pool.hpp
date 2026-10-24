/// @file object_pool.hpp
/// @brief Fixed-capacity object pool with O(1) create / destroy.

#ifndef COMMON_OBJECT_POOL_HPP
#define COMMON_OBJECT_POOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#include "common/assert.hpp"

namespace common {

/// Pre-allocated storage for up to @p Capacity objects of type T. Replaces new/delete on the
/// real-time path: create() and destroy() are O(1) and never touch the system allocator.
///
/// Not thread-safe: the pool belongs to one thread. To hand objects to another thread, send the
/// pointer through an SpscQueue and send it back the same way to be destroyed by the owner.
template <typename T, std::size_t Capacity>
class ObjectPool final {
    static_assert(Capacity > 0U, "Capacity must be positive");
    static_assert(Capacity <= std::numeric_limits<std::uint32_t>::max(), "Capacity must fit in 32 bits");
    static_assert(std::is_nothrow_destructible<T>::value, "T must be nothrow destructible");

public:
    ObjectPool() noexcept {
        // Fill the free list so that slot 0 is handed out first.
        for (std::size_t i{0U}; i < Capacity; ++i) {
            free_list_[i] = static_cast<std::uint32_t>(Capacity - 1U - i);
        }
    }

    ~ObjectPool() {
        for (std::size_t i{0U}; i < Capacity; ++i) {
            if (in_use_[i]) {
                element(i)->~T();
            }
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /// @return the new object, or nullptr when the pool is exhausted.
    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) noexcept {
        static_assert(std::is_nothrow_constructible<T, Args...>::value, "construction must be noexcept");
        T* object{nullptr};
        if (free_count_ > 0U) {
            --free_count_;
            const std::size_t index{free_list_[free_count_]};
            // Deviation (MISRA C++:2023 Rule 21.6.3): placement new into owned, suitably aligned storage.
            object = ::new (slot(index)) T(std::forward<Args>(args)...);
            in_use_[index] = true;
        }
        return object;
    }

    /// Destroys an object obtained from create(). nullptr is ignored.
    /// @pre @p object was created by this pool and not yet destroyed.
    void destroy(T* const object) noexcept {
        if (object != nullptr) {
            const std::size_t index{index_of(object)};
            COMMON_ASSERT(index < Capacity);
            COMMON_ASSERT(in_use_[index]);
            object->~T();
            in_use_[index] = false;
            free_list_[free_count_] = static_cast<std::uint32_t>(index);
            ++free_count_;
        }
    }

    /// @return true if @p object points to a live object of this pool.
    [[nodiscard]] bool owns(const T* const object) const noexcept {
        const std::size_t index{index_of(object)};
        return (index < Capacity) && in_use_[index];
    }

    [[nodiscard]] std::size_t available() const noexcept {
        return free_count_;
    }
    [[nodiscard]] std::size_t in_use() const noexcept {
        return Capacity - free_count_;
    }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    void* slot(const std::size_t index) noexcept {
        return &storage_[index * sizeof(T)];
    }

    // Deviation (MISRA C++:2023 Rule 8.2.5): see StaticVector::element.
    T* element(const std::size_t index) noexcept {
        return std::launder(reinterpret_cast<T*>(&storage_[index * sizeof(T)]));
    }

    /// @return the slot index of @p object, or Capacity if it does not belong to this pool.
    std::size_t index_of(const T* const object) const noexcept {
        std::size_t index{Capacity};
        // Deviation (MISRA C++:2023 Rule 8.2.5): inspect the object's address as bytes.
        const std::byte* const address{reinterpret_cast<const std::byte*>(object)};
        const std::byte* const first{storage_.data()};
        const std::byte* const last{first + storage_.size()};
        // std::less gives a total order even for pointers into unrelated objects.
        const std::less<const std::byte*> less{};
        if ((!less(address, first)) && less(address, last)) {
            const std::size_t offset{static_cast<std::size_t>(address - first)};
            if ((offset % sizeof(T)) == 0U) {
                index = offset / sizeof(T);
            }
        }
        return index;
    }

    alignas(T) std::array<std::byte, sizeof(T) * Capacity> storage_;
    std::array<std::uint32_t, Capacity> free_list_{};
    std::array<bool, Capacity> in_use_{};
    std::size_t free_count_{Capacity};
};

}  // namespace common

#endif  // COMMON_OBJECT_POOL_HPP
