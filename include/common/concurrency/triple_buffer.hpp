/// @file triple_buffer.hpp
/// @brief Wait-free single-writer / single-reader "latest value" channel for any copyable type.

#ifndef COMMON_CONCURRENCY_TRIPLE_BUFFER_HPP
#define COMMON_CONCURRENCY_TRIPLE_BUFFER_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

#include "common/core/config.hpp"

namespace common {

/// Hands the most recent value of T from one writer thread to one reader thread, e.g. a
/// controller's set-point table or an order book snapshot.
///
/// Three slots rotate between the writer, the reader and a shared middle position, exchanged
/// with one atomic operation. Compared with SeqLock:
/// - both sides are wait-free, and a read never fails or retries;
/// - T only needs a nothrow copy assignment, so it may own resources (e.g. a StaticVector);
/// - there is exactly one reader thread.
///
/// The reader works on its own slot, which the writer never touches, so read() returns a
/// reference without copying. Intermediate values the reader did not pick up are skipped.
///
/// The object embeds its storage: give it static storage duration.
///
/// @code
///   // Writer:                         // Reader:
///   buffer.write(snapshot);            if (buffer.update()) { use(buffer.read()); }
/// @endcode
template <typename T>
class TripleBuffer final {
    static_assert(std::is_nothrow_default_constructible<T>::value, "T must be nothrow default-constructible");
    static_assert(std::is_nothrow_copy_assignable<T>::value, "T must be nothrow copy-assignable");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "std::atomic<uint32_t> must be lock-free");

public:
    TripleBuffer() noexcept = default;
    ~TripleBuffer() = default;
    TripleBuffer(const TripleBuffer&) = delete;
    TripleBuffer(TripleBuffer&&) = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;
    TripleBuffer& operator=(TripleBuffer&&) = delete;

    /// Writer only. Publishes a copy of @p value. Never waits for the reader.
    void write(const T& value) noexcept {
        slots_[write_index_].value = value;
        publish();
    }

    /// Writer only. In-place access to the slot the next publish() hands over, to avoid a copy of
    /// a large T. The slot holds an older value, not necessarily the last one written, so
    /// overwrite every field before publishing.
    [[nodiscard]] T& write_buffer() noexcept {
        return slots_[write_index_].value;
    }

    /// Writer only. Publishes the contents of write_buffer().
    void publish() noexcept {
        // Release: the slot contents become visible with the index. Acquire: the slot taken back
        // is no longer read by the reader.
        const std::uint32_t previous{middle_.exchange(write_index_ | kFreshBit, std::memory_order_acq_rel)};
        write_index_ = previous & kIndexMask;
    }

    /// Reader only. Takes the newest published value, if one arrived since the last call.
    /// @return true if read() now returns a newer value.
    bool update() noexcept {
        bool updated{false};
        if ((middle_.load(std::memory_order_relaxed) & kFreshBit) != 0U) {
            const std::uint32_t previous{middle_.exchange(read_index_, std::memory_order_acq_rel)};
            read_index_ = previous & kIndexMask;
            updated = true;
        }
        return updated;
    }

    /// Reader only. The value taken by the last update() (T{} before the first publish).
    /// The reference stays valid and unchanged until the next update().
    [[nodiscard]] const T& read() const noexcept {
        return slots_[read_index_].value;
    }

private:
    static constexpr std::uint32_t kIndexMask{0x3U};
    static constexpr std::uint32_t kFreshBit{0x4U};

    struct alignas(kCacheLineSize) Slot final {
        T value{};
    };

    // Shared: index of the middle slot, plus kFreshBit while it holds an unread value.
    alignas(kCacheLineSize) std::atomic<std::uint32_t> middle_{1U};
    // Writer-owned.
    alignas(kCacheLineSize) std::uint32_t write_index_{0U};
    // Reader-owned.
    alignas(kCacheLineSize) std::uint32_t read_index_{2U};
    std::array<Slot, 3U> slots_{};
};

}  // namespace common

#endif  // COMMON_CONCURRENCY_TRIPLE_BUFFER_HPP
