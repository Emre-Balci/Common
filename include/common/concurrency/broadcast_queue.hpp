/// @file broadcast_queue.hpp
/// @brief Bounded single-producer / multi-consumer broadcast queue with exact loss counts for slow consumers.

#ifndef COMMON_CONCURRENCY_BROADCAST_QUEUE_HPP
#define COMMON_CONCURRENCY_BROADCAST_QUEUE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "common/core/config.hpp"

namespace common {

/// Outcome of BroadcastQueue::try_read().
enum class BroadcastReadStatus : std::uint8_t {
    kOk,     ///< The next message was read.
    kEmpty,  ///< The next message has not been published yet (or is still being written).
    kRetry,  ///< The producer overwrote the message on every attempt. Nothing was read; call again.
};

/// Result of BroadcastQueue::try_read().
struct [[nodiscard]] BroadcastRead final {
    BroadcastReadStatus status{BroadcastReadStatus::kEmpty};
    /// With kOk: the number of messages skipped since the previous read because the producer had
    /// overwritten them, or was about to while this consumer raced it (see BroadcastQueue). Always 0
    /// with kEmpty and kRetry.
    std::uint64_t lost{0U};
};

/// A consumer's position in one BroadcastQueue.
///
/// Messages are numbered 0, 1, 2, ... in publish order. next is the sequence number of the next
/// message the consumer wants, so after a kOk read the message read has sequence number next - 1.
/// Get a cursor from BroadcastQueue::subscribe(). A value-initialised cursor (next == 0) starts at
/// the first message ever published: its first read returns the oldest message still in the queue
/// and counts the older ones as lost.
///
/// A cursor belongs to one consumer thread and is changed only by try_read() on that thread. It is
/// all the state a consumer has, and it fills a cache line of its own, so consumers never write to
/// a cache line that the producer or another consumer uses.
struct alignas(kCacheLineSize) BroadcastCursor final {
    std::uint64_t next{0U};
};

namespace detail {
namespace broadcast_queue {

/// Slot tag while message @p sequence is being written: odd. Tag 0 marks a slot never written.
[[nodiscard]] constexpr std::uint64_t writing_tag(const std::uint64_t sequence) noexcept {
    return (sequence * 2U) + 1U;
}

/// Slot tag once message @p sequence is complete: even and never 0.
[[nodiscard]] constexpr std::uint64_t complete_tag(const std::uint64_t sequence) noexcept {
    return (sequence * 2U) + 2U;
}

/// Outcome of one seqlock read of a slot.
enum class SlotRead : std::uint8_t {
    kCopied,        ///< The wanted message was copied, and no write overlapped the copy.
    kNotPublished,  ///< The slot still holds an older message, or the wanted one is being written.
    kOverwritten,   ///< A later message replaced the wanted one, before or during the copy.
};

/// The message to try after message @p failed was found overwritten, when @p head messages have
/// been published. Every message up to @p failed is gone. The oldest message that may still be in
/// the queue is head - capacity; @p headroom more are skipped so that the next copy can finish
/// before the producer reaches its slot. When head >= failed + 2 (always true after an overrun,
/// where head >= failed + capacity), the result lies in (failed, head - 1].
[[nodiscard]] constexpr std::uint64_t skip_target(const std::uint64_t failed, const std::uint64_t head,
                                                  const std::uint64_t capacity, const std::uint64_t headroom) noexcept {
    const std::uint64_t oldest{(head > capacity) ? (head - capacity) : 0U};
    const std::uint64_t newest{(head > 0U) ? (head - 1U) : 0U};
    const std::uint64_t candidate{((newest - oldest) > headroom) ? (oldest + headroom) : newest};
    return (candidate > failed) ? candidate : (failed + 1U);
}

/// Headroom for the retry after the one that used @p headroom: 0, 1, 2, 4, ... up to capacity - 1.
/// @pre headroom < capacity
[[nodiscard]] constexpr std::uint64_t next_headroom(const std::uint64_t headroom,
                                                    const std::uint64_t capacity) noexcept {
    std::uint64_t next{1U};
    if (headroom > 0U) {
        next = ((headroom * 2U) < (capacity - 1U)) ? (headroom * 2U) : (capacity - 1U);
    }
    return next;
}

}  // namespace broadcast_queue
}  // namespace detail

/// Broadcasts a stream of trivially copyable messages from one producer thread to any number of
/// consumer threads, e.g. market data to several strategies or sensor frames to several functions.
/// Every consumer gets every message in order, unless it falls behind.
///
/// - publish() is wait-free and never waits for a consumer: it always overwrites the oldest of the
///   @p Capacity slots. Consumers do not register and never write to the queue (their only state is
///   their BroadcastCursor), so how many there are and how fast they read does not affect the
///   producer.
/// - Each consumer keeps its own BroadcastCursor and calls try_read(). A consumer that has fallen
///   more than @p Capacity messages behind has lost the oldest ones: try_read() continues with the
///   oldest message still in the queue and reports how many it skipped (BroadcastRead::lost). The
///   accounting is exact: once a consumer has caught up, the messages it read plus the sum of lost
///   equal published() minus the cursor's start.
/// - Every slot is a small seqlock (see SeqLock): the payload is stored as relaxed atomic words and
///   guarded by a per-slot tag, so a message overwritten while it is copied is detected and never
///   returned torn. The implementation is free of data races (and clean under ThreadSanitizer).
/// - try_read() makes at most @p MaxReadAttempts slot reads. When the message at the cursor has
///   been overwritten, the next attempt reads the oldest message still in the queue. If that one
///   is overwritten too, the consumer is racing the producer: each further attempt also skips 1,
///   2, 4, ... (at most Capacity - 1) of the oldest messages, so that the copy can finish before
///   the producer reaches the slot. These messages are counted in lost as well. If every attempt
///   fails, try_read() returns kRetry and changes nothing.
///
/// Message s lives in slot s % Capacity until message s + Capacity replaces it. The slot's tag is
/// 2s + 1 while message s is being written and 2s + 2 once it is complete (0: never written).
/// Sequence numbers are 64-bit. The tags would wrap after 2^63 messages (about 290 years at 10^9
/// messages per second); wrap-around is not handled.
///
/// Threads and real-time use: publish() runs on the one producer thread, try_read() on the thread
/// that owns the cursor, and subscribe() and published() on any thread. Each completes in a bounded
/// number of steps without blocking or allocating, so all of them are safe on the real-time path.
///
/// The object embeds its storage (Capacity slots, each a whole number of cache lines): give it
/// static storage duration, never a real-time thread's stack.
///
/// @code
///   static common::BroadcastQueue<Quote, 1024U> g_quotes{};
///
///   // Producer thread:
///   g_quotes.publish(quote);
///
///   // Each consumer thread:
///   common::BroadcastCursor cursor{g_quotes.subscribe()};
///   Quote quote{};
///   const common::BroadcastRead read{g_quotes.try_read(cursor, quote)};
///   if (read.status == common::BroadcastReadStatus::kOk) {
///       lost_total += read.lost;  // Messages this consumer missed before this one.
///       handle(quote);
///   }
/// @endcode
template <typename T, std::size_t Capacity, std::uint32_t MaxReadAttempts = 16U>
class BroadcastQueue final {
    using Word = std::uint64_t;
    static constexpr std::size_t kWordCount{(sizeof(T) + sizeof(Word) - 1U) / sizeof(Word)};
    static constexpr std::uint64_t kCapacity{Capacity};
    static constexpr std::uint64_t kIndexMask{kCapacity - 1U};

    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    static_assert(std::is_nothrow_default_constructible<T>::value, "T must be nothrow default-constructible");
    static_assert(Capacity >= 2U, "Capacity must be at least 2");
    static_assert(is_power_of_two(Capacity), "Capacity must be a power of two");
    static_assert(MaxReadAttempts >= 2U,
                  "MaxReadAttempts must be at least 2: one to find the cursor overrun, one to read the oldest message");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");

public:
    BroadcastQueue() noexcept = default;
    ~BroadcastQueue() = default;
    BroadcastQueue(const BroadcastQueue&) = delete;
    BroadcastQueue(BroadcastQueue&&) = delete;
    BroadcastQueue& operator=(const BroadcastQueue&) = delete;
    BroadcastQueue& operator=(BroadcastQueue&&) = delete;

    /// Producer only (exactly one thread). Publishes a copy of @p value with sequence number
    /// published(); once the queue is full, it replaces the oldest message. Wait-free: a constant
    /// number of steps, whatever the consumers do.
    void publish(const T& value) noexcept {
        std::array<Word, kWordCount> words{};
        static_cast<void>(std::memcpy(words.data(), &value, sizeof(T)));

        const std::uint64_t sequence{published_.load(std::memory_order_relaxed)};  // Only this thread stores it.
        Slot& slot{slots_[index_of(sequence)]};
        // Release: a consumer that sees this tag also sees published_ >= sequence, which try_read()
        // relies on after an overrun.
        slot.tag.store(detail::broadcast_queue::writing_tag(sequence), std::memory_order_release);
        // Orders the tag before the payload: a consumer whose copy sees any new word also sees the new tag.
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i{0U}; i < kWordCount; ++i) {
            slot.words[i].store(words[i], std::memory_order_relaxed);
        }
        slot.tag.store(detail::broadcast_queue::complete_tag(sequence), std::memory_order_release);
        published_.store(sequence + 1U, std::memory_order_release);
    }

    /// Any thread. Number of messages published so far, which is also the sequence number of the
    /// next one.
    [[nodiscard]] std::uint64_t published() const noexcept {
        return published_.load(std::memory_order_acquire);
    }

    /// Any thread, usually the consumer's own. A cursor at the next message to be published: the
    /// consumer receives only messages published from now on.
    [[nodiscard]] BroadcastCursor subscribe() const noexcept {
        return BroadcastCursor{published_.load(std::memory_order_acquire)};
    }

    /// Consumer that owns @p cursor. Copies the next message after @p cursor into @p out and moves
    /// the cursor past it. At most MaxReadAttempts slot reads; never blocks and never delays the
    /// producer.
    /// @return kOk with the number of messages skipped before this one (lost); kEmpty if the next
    ///         message has not been published yet; kRetry if the producer overwrote the message on
    ///         every attempt. @p out and @p cursor are changed only with kOk.
    [[nodiscard]] BroadcastRead try_read(BroadcastCursor& cursor, T& out) const noexcept {
        BroadcastRead result{BroadcastReadStatus::kRetry, 0U};
        std::array<Word, kWordCount> words{};
        std::uint64_t target{cursor.next};
        std::uint64_t headroom{0U};
        for (std::uint32_t attempt{0U}; (attempt < MaxReadAttempts) && (result.status == BroadcastReadStatus::kRetry);
             ++attempt) {
            const detail::broadcast_queue::SlotRead outcome{read_slot(target, words)};
            if (outcome == detail::broadcast_queue::SlotRead::kCopied) {
                // T is trivially copyable, so copying its bytes is a valid copy. The explicit void*
                // tells GCC (-Wclass-memaccess) that this is intended also when T has default member
                // initialisers.
                static_cast<void>(std::memcpy(static_cast<void*>(&out), words.data(), sizeof(T)));
                result.status = BroadcastReadStatus::kOk;
                result.lost = target - cursor.next;
                cursor.next = target + 1U;
            } else if (outcome == detail::broadcast_queue::SlotRead::kNotPublished) {
                // Only the cursor's own message can be unpublished: a later target is always below a
                // published_ value loaded with acquire, so its slot shows at least its complete tag.
                result.status = BroadcastReadStatus::kEmpty;
            } else {
                // The slot showed a tag of message target + Capacity or later (acquire), so published_
                // is now at least target + Capacity, and the skip lands on a published message. No
                // pause before the next attempt: it reads another slot, and waiting would only let
                // the producer overwrite that one too.
                target = detail::broadcast_queue::skip_target(target, published_.load(std::memory_order_acquire),
                                                              kCapacity, headroom);
                headroom = detail::broadcast_queue::next_headroom(headroom, kCapacity);
            }
        }
        return result;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    struct alignas(kCacheLineSize) Slot final {
        std::atomic<std::uint64_t> tag{0U};
        std::array<std::atomic<Word>, kWordCount> words{};
    };

    [[nodiscard]] static constexpr std::size_t index_of(const std::uint64_t sequence) noexcept {
        const std::size_t index{sequence & kIndexMask};
        return index;
    }

    /// One seqlock read of the slot of message @p sequence into @p words (complete only with kCopied).
    /// Both tag loads are acquire: whatever tag decides the outcome, the consumer then also sees every
    /// store the producer made before writing it, in particular published_.
    [[nodiscard]] detail::broadcast_queue::SlotRead read_slot(const std::uint64_t sequence,
                                                              std::array<Word, kWordCount>& words) const noexcept {
        const Slot& slot{slots_[index_of(sequence)]};
        const std::uint64_t wanted{detail::broadcast_queue::complete_tag(sequence)};
        const std::uint64_t before{slot.tag.load(std::memory_order_acquire)};
        detail::broadcast_queue::SlotRead outcome{detail::broadcast_queue::SlotRead::kOverwritten};
        if (before < wanted) {
            outcome = detail::broadcast_queue::SlotRead::kNotPublished;
        } else if (before == wanted) {
            for (std::size_t i{0U}; i < kWordCount; ++i) {
                words[i] = slot.words[i].load(std::memory_order_relaxed);
            }
            // Orders the payload loads before the second tag load (pairs with the producer's fence).
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.tag.load(std::memory_order_acquire) == wanted) {
                outcome = detail::broadcast_queue::SlotRead::kCopied;
            }
        } else {
            // A later message (sequence + k * Capacity) is being written or is complete: kOverwritten.
        }
        return outcome;
    }

    // Producer-owned cache line: written once per publish(). Consumers load it only to subscribe and
    // to recover from an overrun, so a consumer that keeps up touches only the slots.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> published_{0U};
    std::array<Slot, Capacity> slots_{};
};

}  // namespace common

#endif  // COMMON_CONCURRENCY_BROADCAST_QUEUE_HPP
