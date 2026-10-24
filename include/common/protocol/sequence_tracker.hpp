/// @file sequence_tracker.hpp
/// @brief Detection of lost, repeated, stale and invalid messages from sequence numbers or alive counters.

#ifndef COMMON_PROTOCOL_SEQUENCE_TRACKER_HPP
#define COMMON_PROTOCOL_SEQUENCE_TRACKER_HPP

#include <cstdint>
#include <limits>

#include "common/core/assert.hpp"
#include "common/core/config.hpp"
#include "common/numeric/integer.hpp"

namespace common {

/// How SequenceTracker::check() classifies a counter. delta is the distance from the reference
/// (the last accepted counter) forward to the received counter, modulo 2^CounterBits.
enum class SequenceVerdict : std::uint8_t {
    kFirst,      ///< No reference yet (after construction or reset()): accepted.
    kInOrder,    ///< delta == 1, the expected counter: accepted.
    kGap,        ///< 2 <= delta <= max_lost + 1: delta - 1 messages lost, accepted.
    kJump,       ///< Ahead by more than max_lost + 1: accepted, so the tracker resynchronises.
    kDuplicate,  ///< delta == 0, the reference again: rejected.
    kStale,      ///< delta >= 2^(CounterBits - 1), behind the reference (late or replayed): rejected.
    kInvalid,    ///< Above 2^CounterBits - 1, a value no sender produces: rejected.
};

/// Result of SequenceTracker::check(). A default-constructed value is a rejection (kInvalid).
struct SequenceCheck final {
    SequenceVerdict verdict{SequenceVerdict::kInvalid};
    /// For kGap and kJump, delta - 1: the messages between the reference and this counter that
    /// never arrived (at least; a whole unseen revolution of the counter cannot be detected).
    /// 0 for every other verdict.
    std::uint64_t lost{0U};

    /// @return true if the counter became the new reference (kFirst, kInOrder, kGap, kJump): the
    ///         message is new and may be processed. false for kDuplicate, kStale and kInvalid.
    [[nodiscard]] constexpr bool accepted() const noexcept {
        bool result{false};
        switch (verdict) {
            case SequenceVerdict::kFirst:
            case SequenceVerdict::kInOrder:
            case SequenceVerdict::kGap:
            case SequenceVerdict::kJump:
                result = true;
                break;
            case SequenceVerdict::kDuplicate:
            case SequenceVerdict::kStale:
            case SequenceVerdict::kInvalid:
                break;  // Rejected: keep false.
        }
        return result;
    }
};

/// Counts kept by a SequenceTracker since its construction or the last reset_statistics().
/// checked == accepted + duplicates + stale + invalid; accepted includes the gaps and jumps.
struct SequenceStatistics final {
    std::uint64_t checked{0U};     ///< Calls to check().
    std::uint64_t accepted{0U};    ///< Counters that became the reference (kFirst, kInOrder, kGap, kJump).
    std::uint64_t lost{0U};        ///< Sum of SequenceCheck::lost over gaps and jumps; saturates at 2^64 - 1.
    std::uint64_t gaps{0U};        ///< kGap verdicts.
    std::uint64_t jumps{0U};       ///< kJump verdicts.
    std::uint64_t duplicates{0U};  ///< kDuplicate verdicts.
    std::uint64_t stale{0U};       ///< kStale verdicts.
    std::uint64_t invalid{0U};     ///< kInvalid verdicts.
};

namespace detail {
namespace sequence_tracker {

/// 2^Bits - 1, without ever shifting a 64-bit value by 64 (undefined behaviour).
template <std::uint32_t Bits>
[[nodiscard]] constexpr std::uint64_t max_counter() noexcept {
    std::uint64_t mask{std::numeric_limits<std::uint64_t>::max()};
    if constexpr (Bits < 64U) {
        mask = (std::uint64_t{1U} << Bits) - 1U;
    }
    return mask;
}

}  // namespace sequence_tracker
}  // namespace detail

/// Checks the sequence numbers or alive counters of one message stream and tells, for each
/// message, whether it is new, how many messages were lost before it, or why it must be dropped.
/// Typical streams are market data with 64-bit sequence numbers, where a gap triggers a recovery
/// request, and AUTOSAR E2E-style alive counters of 4, 8, 16 or 32 bits.
///
/// Counters have @p CounterBits bits and wrap from 2^CounterBits - 1 to 0; a range that is not a
/// power of two (e.g. 0 to 14) is not supported. check() compares a counter with the reference,
/// the last accepted counter, through delta = (counter - reference) mod 2^CounterBits:
///
/// | Counter                                     | Verdict    | lost      | Accepted |
/// |---------------------------------------------|------------|-----------|----------|
/// | above 2^CounterBits - 1                     | kInvalid   | 0         | no       |
/// | any other while there is no reference       | kFirst     | 0         | yes      |
/// | delta == 0                                  | kDuplicate | 0         | no       |
/// | delta == 1                                  | kInOrder   | 0         | yes      |
/// | 2 <= delta <= max_lost + 1                  | kGap       | delta - 1 | yes      |
/// | max_lost + 2 <= delta < 2^(CounterBits - 1) | kJump      | delta - 1 | yes      |
/// | delta >= 2^(CounterBits - 1)                | kStale     | 0         | no       |
///
/// An accepted counter becomes the new reference; a rejected one changes nothing but the
/// statistics. The forward half of the counter range counts as ahead of the reference and the
/// backward half as behind it. Consequences:
/// - There is no reordering buffer. A message that arrives after a later one was accepted has
///   been counted in that later message's gap, and is then rejected as kStale.
/// - kJump is accepted so that the tracker resynchronises after a sender restart or a long
///   outage; the application decides how to react, e.g. by requesting a snapshot.
/// - A counter cannot show more than 2^(CounterBits - 1) - 2 lost messages: a longer outage looks
///   like a smaller gap, a duplicate or stale messages. Choose CounterBits accordingly, and
///   supervise the reception time as well (E2E does both).
/// - A sender that restarts with a lower counter is stale until its counters pass the reference
///   (with 64-bit sequence numbers, practically never). Call reset() when the protocol announces
///   a new session or a sequence reset. To continue after a recovery snapshot with sequence
///   number S, call reset() and then check(S): every message up to S is then rejected. (This
///   counts as one checked and accepted message in the statistics.)
///
/// In AUTOSAR E2E terms, with MaxDeltaCounter = max_lost + 1, kInOrder, kGap and kDuplicate
/// correspond to the statuses OK, OKSOMELOST and REPEATED, and kJump and kStale to WRONGSEQUENCE.
///
/// RT-path safe: check() is O(1), a few comparisons without a loop, and no operation allocates,
/// blocks or throws. Not thread-safe: the tracker belongs to the thread that receives the stream.
///
/// @code
///   // Market data: up to 1000 missing messages are recovered by retransmission.
///   common::SequenceTracker<64U> feed{1000U};
///   const common::SequenceCheck result{feed.check(packet.sequence)};
///   if (result.verdict == common::SequenceVerdict::kGap) {
///       request_retransmission(packet.sequence - result.lost, result.lost);  // First missing, count.
///   } else if (result.verdict == common::SequenceVerdict::kJump) {
///       request_snapshot();
///   } else {
///       // First, in order, or rejected: nothing to recover.
///   }
///   if (result.accepted()) {
///       apply(packet);
///   }
///
///   // E2E-style 4-bit alive counter: up to 2 consecutive frames may be lost.
///   common::SequenceTracker<4U> alive{2U};
///   const bool fresh{alive.check(frame.counter).accepted()};
/// @endcode
template <std::uint32_t CounterBits>
class SequenceTracker final {
    static_assert((CounterBits >= 2U) && (CounterBits <= 64U), "CounterBits must be in [2, 64]");

public:
    /// @p max_lost is the largest number of consecutive lost messages that still counts as an
    /// ordinary gap (kGap); a longer run is a kJump. With 0, every loss is a jump.
    /// @pre max_lost <= max_lost_limit(), so that every gap lies in the forward half of the
    ///      counter range. Checked with COMMON_ASSERT; if assertions are disabled, a larger value
    ///      is clamped to max_lost_limit().
    explicit SequenceTracker(const std::uint64_t max_lost) noexcept
        : max_lost_{(max_lost <= kMaxLostLimit) ? max_lost : kMaxLostLimit} {
        COMMON_ASSERT(max_lost <= kMaxLostLimit);
    }

    /// Classifies @p counter as the class description states, makes it the new reference if it
    /// is accepted, and updates the statistics. Every value is allowed: one above max_counter() is
    /// reported as kInvalid, never asserted, so counters from the network or bus can be passed
    /// directly. O(1).
    [[nodiscard]] SequenceCheck check(const std::uint64_t counter) noexcept {
        const SequenceCheck result{classify(counter)};
        if (result.accepted()) {
            reference_ = counter;
            has_reference_ = true;
        }
        record(result);
        return result;
    }

    /// @return true once a counter has been accepted since construction or reset().
    [[nodiscard]] bool has_reference() const noexcept {
        return has_reference_;
    }

    /// @return the counter that check() would report as kInOrder: the reference + 1, which wraps
    ///         to 0 after max_counter().
    /// @pre has_reference()
    [[nodiscard]] std::uint64_t expected() const noexcept {
        COMMON_ASSERT(has_reference_);
        return (reference_ + 1U) & kMaxCounter;  // Wraps like the sender's counter.
    }

    /// Forgets the reference, so that the next valid counter is kFirst. The statistics are kept.
    void reset() noexcept {
        reference_ = 0U;
        has_reference_ = false;
    }

    [[nodiscard]] const SequenceStatistics& statistics() const noexcept {
        return statistics_;
    }

    /// Sets every count to 0. The reference is kept.
    void reset_statistics() noexcept {
        statistics_ = SequenceStatistics{};
    }

    /// Largest number of lost messages that is reported as kGap rather than kJump.
    [[nodiscard]] std::uint64_t max_lost() const noexcept {
        return max_lost_;
    }

    /// Largest valid counter: 2^CounterBits - 1.
    [[nodiscard]] static constexpr std::uint64_t max_counter() noexcept {
        return kMaxCounter;
    }

    /// Largest max_lost the constructor accepts: 2^(CounterBits - 1) - 2 (0 for 2-bit counters).
    [[nodiscard]] static constexpr std::uint64_t max_lost_limit() noexcept {
        return kMaxLostLimit;
    }

private:
    static constexpr std::uint64_t kMaxCounter{detail::sequence_tracker::max_counter<CounterBits>()};
    // 2^(CounterBits - 1): every delta from here up lies in the backward half of the range.
    static constexpr std::uint64_t kHalfRange{(kMaxCounter >> 1U) + 1U};
    // The largest forward delta, kHalfRange - 1, means kHalfRange - 2 lost messages.
    static constexpr std::uint64_t kMaxLostLimit{kHalfRange - 2U};

    [[nodiscard]] SequenceCheck classify(const std::uint64_t counter) const noexcept {
        SequenceCheck result{};
        if (counter > kMaxCounter) {
            result.verdict = SequenceVerdict::kInvalid;
        } else if (!has_reference_) {
            result.verdict = SequenceVerdict::kFirst;
        } else {
            // Distance modulo 2^CounterBits: the unsigned wrap-around of the subtraction is intended.
            const std::uint64_t delta{(counter - reference_) & kMaxCounter};
            if (delta == 0U) {
                result.verdict = SequenceVerdict::kDuplicate;
            } else if (delta == 1U) {
                result.verdict = SequenceVerdict::kInOrder;
            } else if (delta >= kHalfRange) {
                result.verdict = SequenceVerdict::kStale;
            } else {
                result.lost = delta - 1U;
                result.verdict = (result.lost <= max_lost_) ? SequenceVerdict::kGap : SequenceVerdict::kJump;
            }
        }
        return result;
    }

    void record(const SequenceCheck& result) noexcept {
        ++statistics_.checked;
        if (result.accepted()) {
            ++statistics_.accepted;
        }
        // A single 64-bit jump can report up to 2^63 - 2 lost messages, so the sum saturates.
        statistics_.lost = saturating_add(statistics_.lost, result.lost);
        switch (result.verdict) {
            case SequenceVerdict::kFirst:
            case SequenceVerdict::kInOrder:
                break;  // Counted in checked and accepted only.
            case SequenceVerdict::kGap:
                ++statistics_.gaps;
                break;
            case SequenceVerdict::kJump:
                ++statistics_.jumps;
                break;
            case SequenceVerdict::kDuplicate:
                ++statistics_.duplicates;
                break;
            case SequenceVerdict::kStale:
                ++statistics_.stale;
                break;
            case SequenceVerdict::kInvalid:
                ++statistics_.invalid;
                break;
        }
    }

    std::uint64_t max_lost_;
    std::uint64_t reference_{0U};  // The last accepted counter; meaningful while has_reference_.
    bool has_reference_{false};
    SequenceStatistics statistics_{};
};

}  // namespace common

#endif  // COMMON_PROTOCOL_SEQUENCE_TRACKER_HPP
