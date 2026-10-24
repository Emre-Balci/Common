/// @file seqlock.hpp
/// @brief Single-writer / multi-reader "latest value" channel.

#ifndef COMMON_SEQLOCK_HPP
#define COMMON_SEQLOCK_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "common/config.hpp"
#include "common/spin.hpp"

namespace common {

/// Publishes the most recent value of a trivially-copyable T (e.g. a market data snapshot or a
/// sensor state) from one writer to any number of readers.
///
/// - The writer is wait-free and never waits for readers.
/// - Readers never block the writer; a read retries at most @p MaxReadAttempts times if it
///   overlaps a write, then reports failure.
/// - The payload is stored as relaxed atomic words, so the implementation is free of data races
///   under the C++ memory model (and clean under ThreadSanitizer).
template <typename T, std::uint32_t MaxReadAttempts = 16U>
class SeqLock final {
    using Word = std::uint64_t;
    static constexpr std::size_t kWordCount{(sizeof(T) + sizeof(Word) - 1U) / sizeof(Word)};

    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    static_assert(std::is_nothrow_default_constructible<T>::value, "T must be nothrow default-constructible");
    static_assert(MaxReadAttempts > 0U, "MaxReadAttempts must be positive");
    static_assert(std::atomic<Word>::is_always_lock_free, "64-bit atomics must be lock-free");

public:
    SeqLock() noexcept {
        store(T{});
    }
    ~SeqLock() = default;
    SeqLock(const SeqLock&) = delete;
    SeqLock(SeqLock&&) = delete;
    SeqLock& operator=(const SeqLock&) = delete;
    SeqLock& operator=(SeqLock&&) = delete;

    /// Writer only (a single thread).
    void store(const T& value) noexcept {
        std::array<Word, kWordCount> words{};
        static_cast<void>(std::memcpy(words.data(), &value, sizeof(T)));

        const std::size_t seq{sequence_.load(std::memory_order_relaxed)};
        sequence_.store(seq + 1U, std::memory_order_relaxed);  // Odd: write in progress.
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i{0U}; i < kWordCount; ++i) {
            data_[i].store(words[i], std::memory_order_relaxed);
        }
        sequence_.store(seq + 2U, std::memory_order_release);  // Even: stable.
    }

    /// Any thread. @return false if every attempt overlapped a write (@p out is left untouched).
    [[nodiscard]] bool try_load(T& out) const noexcept {
        for (std::uint32_t attempt{0U}; attempt < MaxReadAttempts; ++attempt) {
            const std::size_t before{sequence_.load(std::memory_order_acquire)};
            if ((before & 1U) == 0U) {
                std::array<Word, kWordCount> words{};
                for (std::size_t i{0U}; i < kWordCount; ++i) {
                    words[i] = data_[i].load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                const std::size_t after{sequence_.load(std::memory_order_relaxed)};
                if (before == after) {
                    static_cast<void>(std::memcpy(&out, words.data(), sizeof(T)));
                    return true;
                }
            }
            cpu_relax();
        }
        return false;
    }

    /// Number of completed stores, including the initial default value. Readers can compare
    /// versions to detect whether a new value has been published.
    [[nodiscard]] std::size_t version() const noexcept {
        return sequence_.load(std::memory_order_acquire) / 2U;
    }

private:
    alignas(kCacheLineSize) std::atomic<std::size_t> sequence_{0U};
    std::array<std::atomic<Word>, kWordCount> data_{};
};

}  // namespace common

#endif  // COMMON_SEQLOCK_HPP
