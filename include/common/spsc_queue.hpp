/// @file spsc_queue.hpp
/// @brief Bounded, wait-free single-producer / single-consumer queue.

#ifndef COMMON_SPSC_QUEUE_HPP
#define COMMON_SPSC_QUEUE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "common/config.hpp"

namespace common {

/// Wait-free ring buffer for exactly one producer thread and one consumer thread.
///
/// - All operations complete in a bounded number of steps and never block or allocate.
/// - Every one of the @p Capacity slots is usable (indices are free-running counters).
/// - Producer and consumer state live on separate cache lines; each side caches the other's
///   index so the shared line is only touched when the queue looks full / empty.
///
/// The object embeds its storage: give it static storage duration or allocate it during
/// initialisation, never on a real-time thread's stack.
template <typename T, std::size_t Capacity>
class SpscQueue final {
    static_assert(Capacity >= 2U, "Capacity must be at least 2");
    static_assert(is_power_of_two(Capacity), "Capacity must be a power of two");
    static_assert(std::is_nothrow_default_constructible<T>::value, "T must be nothrow default-constructible");
    static_assert(std::is_nothrow_copy_assignable<T>::value, "T must be nothrow copy-assignable");
    static_assert(std::is_nothrow_move_assignable<T>::value, "T must be nothrow move-assignable");
    static_assert(std::atomic<std::size_t>::is_always_lock_free, "std::atomic<size_t> must be lock-free");

public:
    SpscQueue() noexcept = default;
    ~SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    /// Producer only. @return false if the queue is full (the value is not consumed).
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const std::size_t tail{tail_.load(std::memory_order_relaxed)};
        bool pushed{false};
        if (has_space(tail)) {
            buffer_[tail & kMask] = value;
            tail_.store(tail + 1U, std::memory_order_release);
            pushed = true;
        }
        return pushed;
    }

    /// Producer only. @return false if the queue is full (@p value is left untouched).
    [[nodiscard]] bool try_push(T&& value) noexcept {
        const std::size_t tail{tail_.load(std::memory_order_relaxed)};
        bool pushed{false};
        if (has_space(tail)) {
            buffer_[tail & kMask] = std::move(value);
            tail_.store(tail + 1U, std::memory_order_release);
            pushed = true;
        }
        return pushed;
    }

    /// Consumer only. @return false if the queue is empty (@p out is left untouched).
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t head{head_.load(std::memory_order_relaxed)};
        bool popped{false};
        if (has_data(head)) {
            out = std::move(buffer_[head & kMask]);
            head_.store(head + 1U, std::memory_order_release);
            popped = true;
        }
        return popped;
    }

    /// Consumer only. Zero-copy access to the oldest element; nullptr when empty.
    /// The pointer stays valid until pop() is called.
    [[nodiscard]] const T* front() noexcept {
        const std::size_t head{head_.load(std::memory_order_relaxed)};
        return has_data(head) ? &buffer_[head & kMask] : nullptr;
    }

    /// Consumer only. Discards the element returned by front().
    /// @pre front() != nullptr
    void pop() noexcept {
        const std::size_t head{head_.load(std::memory_order_relaxed)};
        head_.store(head + 1U, std::memory_order_release);
    }

    /// Either thread. The value may be stale by the time it is used.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        // Load head first: it can only be <= the tail loaded afterwards.
        const std::size_t head{head_.load(std::memory_order_acquire)};
        const std::size_t tail{tail_.load(std::memory_order_acquire)};
        const std::size_t size{tail - head};
        return (size > Capacity) ? Capacity : size;
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return size_approx() == 0U;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr std::size_t kMask{Capacity - 1U};

    bool has_space(const std::size_t tail) noexcept {
        if ((tail - cached_head_) >= Capacity) {
            cached_head_ = head_.load(std::memory_order_acquire);
        }
        return (tail - cached_head_) < Capacity;
    }

    bool has_data(const std::size_t head) noexcept {
        if (head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
        }
        return head != cached_tail_;
    }

    // Consumer-owned cache line.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0U};
    std::size_t cached_tail_{0U};

    // Producer-owned cache line.
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0U};
    std::size_t cached_head_{0U};

    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

}  // namespace common

#endif  // COMMON_SPSC_QUEUE_HPP
