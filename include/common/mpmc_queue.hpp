/// @file mpmc_queue.hpp
/// @brief Bounded, lock-free multi-producer / multi-consumer queue (Vyukov algorithm).

#ifndef COMMON_MPMC_QUEUE_HPP
#define COMMON_MPMC_QUEUE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "common/config.hpp"

namespace common {

/// Lock-free bounded queue for any number of producers and consumers.
///
/// Each slot carries a sequence number, so producers and consumers only contend on the
/// enqueue / dequeue counters. Contention is bounded: after @p MaxAttempts failed CAS
/// rounds an operation gives up and returns false. A false return therefore means
/// "full / empty, or too contended right now"; the caller decides whether to retry.
///
/// If a producer is preempted between claiming a slot and publishing it, consumers see that
/// slot as empty until the producer resumes; no operation ever blocks on it.
///
/// The object embeds its storage: give it static storage duration.
template <typename T, std::size_t Capacity, std::uint32_t MaxAttempts = 64U>
class MpmcQueue final {
    static_assert(Capacity >= 2U, "Capacity must be at least 2");
    static_assert(is_power_of_two(Capacity), "Capacity must be a power of two");
    static_assert(MaxAttempts > 0U, "MaxAttempts must be positive");
    static_assert(std::is_nothrow_default_constructible<T>::value, "T must be nothrow default-constructible");
    static_assert(std::is_nothrow_copy_assignable<T>::value, "T must be nothrow copy-assignable");
    static_assert(std::is_nothrow_move_assignable<T>::value, "T must be nothrow move-assignable");
    static_assert(std::atomic<std::size_t>::is_always_lock_free, "std::atomic<size_t> must be lock-free");

public:
    MpmcQueue() noexcept {
        for (std::size_t i{0U}; i < Capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }
    ~MpmcQueue() = default;
    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue(MpmcQueue&&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;
    MpmcQueue& operator=(MpmcQueue&&) = delete;

    [[nodiscard]] bool try_push(const T& value) noexcept {
        return push_impl(value);
    }

    [[nodiscard]] bool try_push(T&& value) noexcept {
        return push_impl(std::move(value));
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        std::size_t pos{dequeue_pos_.load(std::memory_order_relaxed)};
        for (std::uint32_t attempt{0U}; attempt < MaxAttempts; ++attempt) {
            Cell& cell{cells_[pos & kMask]};
            const std::size_t seq{cell.sequence.load(std::memory_order_acquire)};
            const std::size_t expected{pos + 1U};
            if (seq == expected) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1U, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                    out = std::move(cell.data);
                    cell.sequence.store(pos + Capacity, std::memory_order_release);
                    return true;
                }
                // CAS failure reloaded pos.
            } else if (is_before(seq, expected)) {
                return false;  // Slot not yet written: empty.
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        return false;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr std::size_t kMask{Capacity - 1U};

    struct alignas(kCacheLineSize) Cell final {
        std::atomic<std::size_t> sequence{0U};
        T data{};
    };

    /// Wrap-around safe "a < b" for free-running counters. The unsigned-to-signed conversion is
    /// modular on all supported compilers (and guaranteed so from C++20).
    static constexpr bool is_before(const std::size_t a, const std::size_t b) noexcept {
        return static_cast<std::ptrdiff_t>(a - b) < 0;
    }

    template <typename U>
    bool push_impl(U&& value) noexcept {
        std::size_t pos{enqueue_pos_.load(std::memory_order_relaxed)};
        for (std::uint32_t attempt{0U}; attempt < MaxAttempts; ++attempt) {
            Cell& cell{cells_[pos & kMask]};
            const std::size_t seq{cell.sequence.load(std::memory_order_acquire)};
            if (seq == pos) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1U, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                    cell.data = std::forward<U>(value);
                    cell.sequence.store(pos + 1U, std::memory_order_release);
                    return true;
                }
                // CAS failure reloaded pos.
            } else if (is_before(seq, pos)) {
                return false;  // Slot still holds an unconsumed element: full.
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        return false;
    }

    alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_{0U};
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_{0U};
    std::array<Cell, Capacity> cells_{};
};

}  // namespace common

#endif  // COMMON_MPMC_QUEUE_HPP
