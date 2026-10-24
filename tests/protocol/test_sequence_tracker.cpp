#include <cstdint>
#include <limits>

#include "common/protocol/sequence_tracker.hpp"
#include "test_framework.hpp"

namespace {

using Verdict = common::SequenceVerdict;

/// The verdict the class table specifies for a counter checked against @p reference.
Verdict expected_verdict(const std::uint64_t reference, const std::uint64_t counter, const std::uint64_t max_lost,
                         const std::uint64_t modulus) {
    Verdict verdict{Verdict::kInvalid};
    if (counter < modulus) {
        const std::uint64_t delta{(counter + modulus - reference) % modulus};
        if (delta == 0U) {
            verdict = Verdict::kDuplicate;
        } else if (delta == 1U) {
            verdict = Verdict::kInOrder;
        } else if (delta >= (modulus / 2U)) {
            verdict = Verdict::kStale;
        } else if ((delta - 1U) <= max_lost) {
            verdict = Verdict::kGap;
        } else {
            verdict = Verdict::kJump;
        }
    }
    return verdict;
}

}  // namespace

TEST_CASE(sequence_tracker_market_data_feed) {
    common::SequenceTracker<64U> feed{1000U};
    CHECK(!feed.has_reference());
    CHECK(feed.check(100U).verdict == Verdict::kFirst);
    CHECK(feed.has_reference());
    CHECK(feed.expected() == 101U);
    CHECK(feed.check(101U).verdict == Verdict::kInOrder);

    const common::SequenceCheck gap{feed.check(105U)};  // 102, 103 and 104 are missing.
    CHECK(gap.verdict == Verdict::kGap);
    CHECK(gap.lost == 3U);
    CHECK(gap.accepted());
    CHECK(feed.check(105U).verdict == Verdict::kDuplicate);
    const common::SequenceCheck late{feed.check(103U)};  // Counted in the gap already.
    CHECK(late.verdict == Verdict::kStale);
    CHECK(late.lost == 0U);
    CHECK(!late.accepted());

    const common::SequenceCheck jump{feed.check(105U + 1002U)};
    CHECK(jump.verdict == Verdict::kJump);  // 1001 lost: more than max_lost.
    CHECK(jump.lost == 1001U);
    CHECK(feed.check(105U + 1002U + 1001U).verdict == Verdict::kGap);  // Exactly max_lost lost.

    const common::SequenceStatistics& stats{feed.statistics()};
    CHECK(stats.checked == 7U);
    CHECK(stats.accepted == 5U);
    CHECK(stats.gaps == 2U);
    CHECK(stats.jumps == 1U);
    CHECK(stats.duplicates == 1U);
    CHECK(stats.stale == 1U);
    CHECK(stats.invalid == 0U);
    CHECK(stats.lost == (3U + 1001U + 1000U));
    CHECK(stats.checked == (stats.accepted + stats.duplicates + stats.stale + stats.invalid));

    // Recovery snapshot at sequence 5000: everything up to it is rejected afterwards.
    feed.reset();
    CHECK(!feed.has_reference());
    CHECK(feed.check(5000U).verdict == Verdict::kFirst);
    CHECK(feed.check(4999U).verdict == Verdict::kStale);
    CHECK(feed.check(5000U).verdict == Verdict::kDuplicate);
    CHECK(feed.statistics().checked == 10U);  // reset() keeps the statistics.
    feed.reset_statistics();
    CHECK(feed.statistics().checked == 0U);
    CHECK(feed.has_reference());  // reset_statistics() keeps the reference.
    CHECK(feed.check(5001U).verdict == Verdict::kInOrder);
}

TEST_CASE(sequence_tracker_alive_counter_wraps) {
    common::SequenceTracker<4U> alive{2U};
    CHECK(common::SequenceTracker<4U>::max_counter() == 15U);
    CHECK(common::SequenceTracker<4U>::max_lost_limit() == 6U);
    CHECK(alive.max_lost() == 2U);
    CHECK(alive.check(15U).verdict == Verdict::kFirst);
    CHECK(alive.expected() == 0U);  // Wraps like the sender's counter.
    CHECK(alive.check(0U).verdict == Verdict::kInOrder);
    CHECK(alive.check(3U).lost == 2U);  // 1 and 2 lost: a gap.
    CHECK(alive.check(7U).verdict == Verdict::kJump);  // 3 lost.
    CHECK(alive.check(15U).verdict == Verdict::kStale);  // delta 8: the backward half.
    CHECK(alive.check(16U).verdict == Verdict::kInvalid);  // No 4-bit sender produces it.
    CHECK(alive.check(std::numeric_limits<std::uint64_t>::max()).verdict == Verdict::kInvalid);
    CHECK(alive.statistics().invalid == 2U);

    // With max_lost 0 every loss is a jump.
    common::SequenceTracker<8U> strict{0U};
    CHECK(strict.check(10U).verdict == Verdict::kFirst);
    CHECK(strict.check(12U).verdict == Verdict::kJump);
    CHECK(common::SequenceTracker<8U>::max_lost_limit() == 126U);

    // The smallest counter: 2 bits, where only in-order, duplicate and stale exist.
    common::SequenceTracker<2U> tiny{0U};
    CHECK(common::SequenceTracker<2U>::max_lost_limit() == 0U);
    CHECK(tiny.check(3U).verdict == Verdict::kFirst);
    CHECK(tiny.check(0U).verdict == Verdict::kInOrder);
    CHECK(tiny.check(2U).verdict == Verdict::kStale);
    CHECK(tiny.check(0U).verdict == Verdict::kDuplicate);
}

TEST_CASE(sequence_tracker_matches_the_table_exhaustively) {
    constexpr std::uint64_t kModulus{16U};
    std::uint32_t mismatches{0U};
    for (std::uint64_t max_lost{0U}; max_lost <= common::SequenceTracker<4U>::max_lost_limit(); ++max_lost) {
        for (std::uint64_t reference{0U}; reference < kModulus; ++reference) {
            for (std::uint64_t counter{0U}; counter < (kModulus + 2U); ++counter) {
                common::SequenceTracker<4U> tracker{max_lost};
                static_cast<void>(tracker.check(reference));
                const common::SequenceCheck result{tracker.check(counter)};
                const Verdict expected{expected_verdict(reference, counter, max_lost, kModulus)};
                const bool lost_ok{((expected == Verdict::kGap) || (expected == Verdict::kJump))
                                       ? (result.lost == (((counter + kModulus) - reference) % kModulus) - 1U)
                                       : (result.lost == 0U)};
                mismatches += ((result.verdict == expected) && lost_ok) ? 0U : 1U;
                // A rejected counter leaves the reference unchanged.
                const std::uint64_t reference_after{result.accepted() ? counter : reference};
                mismatches += (tracker.expected() == ((reference_after + 1U) % kModulus)) ? 0U : 1U;
            }
        }
    }
    CHECK(mismatches == 0U);
}

TEST_CASE(sequence_tracker_64_bit_extremes) {
    constexpr std::uint64_t kMaxCounter{std::numeric_limits<std::uint64_t>::max()};
    CHECK(common::SequenceTracker<64U>::max_counter() == kMaxCounter);
    CHECK(common::SequenceTracker<64U>::max_lost_limit() == ((std::uint64_t{1U} << 63U) - 2U));

    common::SequenceTracker<64U> feed{10U};
    CHECK(feed.check(kMaxCounter).verdict == Verdict::kFirst);
    CHECK(feed.expected() == 0U);
    CHECK(feed.check(0U).verdict == Verdict::kInOrder);  // Wraps.

    // The largest forward delta is a jump; one more is stale.
    const std::uint64_t half{std::uint64_t{1U} << 63U};
    CHECK(feed.check(half - 1U).verdict == Verdict::kJump);
    CHECK(feed.check(half - 1U + half).verdict == Verdict::kStale);

    // The lost sum saturates instead of wrapping.
    for (std::uint32_t i{0U}; i < 4U; ++i) {
        static_cast<void>(feed.check(feed.expected() + (half - 2U)));
    }
    CHECK(feed.statistics().lost == kMaxCounter);

    const common::SequenceCheck rejected{};
    CHECK(rejected.verdict == Verdict::kInvalid);
    CHECK(!rejected.accepted());
}
