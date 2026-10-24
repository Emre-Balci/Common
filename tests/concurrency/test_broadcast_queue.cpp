#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "common/concurrency/broadcast_queue.hpp"
#include "common/concurrency/spin.hpp"
#include "test_framework.hpp"

namespace {

using ReadStatus = common::BroadcastReadStatus;

/// A sequence number and a check word derived from it, so a wrong or mixed-up copy is visible.
struct Message final {
    std::uint64_t sequence{0U};
    std::uint64_t check{0U};
};

constexpr std::uint64_t kCheckKey{0x9E37'79B9'7F4A'7C15U};

Message make_message(const std::uint64_t sequence) noexcept {
    return Message{sequence, sequence ^ kCheckKey};
}

bool is_message(const Message& message, const std::uint64_t sequence) noexcept {
    return (message.sequence == sequence) && (message.check == (sequence ^ kCheckKey));
}

/// Publishes messages first, first + 1, ..., first + count - 1.
template <typename Queue>
void publish_range(Queue& queue, const std::uint64_t first, const std::uint64_t count) noexcept {
    for (std::uint64_t sequence{first}; sequence < (first + count); ++sequence) {
        queue.publish(make_message(sequence));
    }
}

/// @return true if the next read returns message @p expected and reports @p lost skipped messages.
template <typename Queue>
bool reads(const Queue& queue, common::BroadcastCursor& cursor, const std::uint64_t expected,
           const std::uint64_t lost) noexcept {
    Message out{};
    const common::BroadcastRead read{queue.try_read(cursor, out)};
    return (read.status == ReadStatus::kOk) && (read.lost == lost) && is_message(out, expected) &&
           (cursor.next == (expected + 1U));
}

/// @return true if the next read reports kEmpty and leaves the cursor and the output untouched.
template <typename Queue>
bool reads_empty(const Queue& queue, common::BroadcastCursor& cursor) noexcept {
    constexpr std::uint64_t kMarker{0xDEAD'BEEFU};
    Message out{make_message(kMarker)};
    const std::uint64_t before{cursor.next};
    const common::BroadcastRead read{queue.try_read(cursor, out)};
    return (read.status == ReadStatus::kEmpty) && (read.lost == 0U) && (cursor.next == before) &&
           is_message(out, kMarker);
}

/// Messages a consumer received and lost.
struct Tally final {
    std::uint64_t received{0U};
    std::uint64_t lost{0U};
    bool consistent{true};  ///< Every read returned the message its lost count implies, and it ended empty.
};

/// Reads until kEmpty. Single-threaded, so each read succeeds or reports kEmpty: after the first
/// read at most capacity() messages are left, and capacity() + 1 reads always suffice.
template <typename Queue>
Tally drain(const Queue& queue, common::BroadcastCursor& cursor) noexcept {
    Tally tally{};
    bool more{true};
    for (std::size_t i{0U}; more && (i <= Queue::capacity()); ++i) {
        const std::uint64_t before{cursor.next};
        Message out{};
        const common::BroadcastRead read{queue.try_read(cursor, out)};
        if (read.status == ReadStatus::kOk) {
            tally.consistent = tally.consistent && is_message(out, before + read.lost);
            ++tally.received;
            tally.lost += read.lost;
        } else {
            tally.consistent = tally.consistent && (read.status == ReadStatus::kEmpty);
            more = false;
        }
    }
    tally.consistent = tally.consistent && (!more);
    return tally;
}

/// Reference model of one consumer: the next read returns message max(next, published - Capacity)
/// and reports the messages before it as lost; with next == published it reports kEmpty.
struct ModelConsumer final {
    std::uint64_t next{0U};
    std::uint64_t start{0U};
    Tally tally{};
};

/// Applies one try_read() result to @p model. @return true if it is the result the model predicts.
bool check_against_model(ModelConsumer& model, const common::BroadcastRead& read, const Message& out,
                         const std::uint64_t cursor_next, const std::uint64_t published,
                         const std::uint64_t capacity) noexcept {
    bool matches{false};
    if (model.next == published) {
        matches = (read.status == ReadStatus::kEmpty) && (read.lost == 0U) && (cursor_next == published);
    } else {
        const std::uint64_t oldest{(published > capacity) ? (published - capacity) : 0U};
        const std::uint64_t expected{(model.next > oldest) ? model.next : oldest};
        matches = (read.status == ReadStatus::kOk) && (read.lost == (expected - model.next)) &&
                  is_message(out, expected) && (cursor_next == (expected + 1U));
        ++model.tally.received;
        model.tally.lost += read.lost;
        model.next = expected + 1U;
    }
    return matches;
}

/// 12 bytes: the second payload word is only half used.
struct Packed12 final {
    std::uint32_t a{0U};
    std::uint32_t b{0U};
    std::uint32_t c{0U};
};

/// 160 bytes: with its tag, a slot spans three cache lines.
struct Wide final {
    std::array<std::uint64_t, 20U> words{};
};

/// Floating-point payload without padding, compared bit for bit.
struct Quote final {
    double price{0.0};
    std::int32_t quantity{0};
    std::uint32_t flags{0U};
};

/// 72 bytes: with its tag, a slot spans two cache lines. Every word holds the sequence number, so a
/// copy that mixes two messages is detectable.
struct Frame final {
    std::array<std::uint64_t, 9U> words{};
};

Frame make_frame(const std::uint64_t sequence) noexcept {
    Frame frame{};
    frame.words.fill(sequence);
    return frame;
}

bool is_untorn(const Frame& frame) noexcept {
    bool same{true};
    for (const std::uint64_t word : frame.words) {
        same = same && (word == frame.words[0U]);
    }
    return same;
}

constexpr std::size_t kStressConsumers{3U};
constexpr std::uint64_t kStressMessages{200'000U};
/// The producer alternates full-speed bursts with stretches paced by a short pause per message.
constexpr std::uint64_t kStressStretch{1'000U};
constexpr std::uint32_t kProducerPauseSpins{32U};
/// Busy work after each message read: a fast, a medium and a slow consumer.
constexpr std::array<std::uint32_t, kStressConsumers> kConsumerWorkSpins{0U, 64U, 2'048U};

/// What one stress consumer saw. errors counts every violated expectation.
struct ConsumerReport final {
    std::uint64_t start{0U};
    std::uint64_t received{0U};
    std::uint64_t lost{0U};
    std::uint64_t last{0U};
    std::uint64_t retries{0U};
    std::uint64_t errors{0U};
};

template <typename Queue>
void consume(const Queue& queue, const std::atomic<bool>& done, std::atomic<std::size_t>& subscribed,
             const std::uint32_t work_spins, ConsumerReport& report) noexcept {
    common::BroadcastCursor cursor{queue.subscribe()};
    report.start = cursor.next;
    subscribed.fetch_add(1U, std::memory_order_release);
    Frame frame{};
    bool caught_up{false};
    while (!caught_up) {
        // Load done before reading: an empty read after the producer finished means nothing is left.
        const bool producer_done{done.load(std::memory_order_acquire)};
        const std::uint64_t before{cursor.next};
        const Frame previous{frame};
        const common::BroadcastRead read{queue.try_read(cursor, frame)};
        if (read.status == ReadStatus::kOk) {
            // The payload names the message: it must be untorn, be the one the lost count implies,
            // and come strictly after the previous one.
            const std::uint64_t sequence{frame.words[0U]};
            const bool valid{is_untorn(frame) && (sequence == (before + read.lost)) &&
                             (cursor.next == (sequence + 1U)) && ((report.received == 0U) || (sequence > report.last))};
            report.errors += valid ? 0U : 1U;
            report.last = sequence;
            ++report.received;
            report.lost += read.lost;
            for (std::uint32_t spin{0U}; spin < work_spins; ++spin) {
                common::cpu_relax();
            }
        } else {
            // Nothing was read: the cursor and the output must be untouched.
            const bool untouched{(cursor.next == before) && (frame.words == previous.words) && (read.lost == 0U)};
            report.errors += untouched ? 0U : 1U;
            if (read.status == ReadStatus::kRetry) {
                ++report.retries;
            } else {
                caught_up = producer_done;
            }
        }
    }
}

/// One producer and three consumers of different speeds. Only scheduling-independent facts are
/// checked: no torn or misnumbered message, strictly increasing sequence numbers, nothing changed
/// by a read that returns no message, exact loss accounting, and that every consumer ends with the
/// newest message.
template <std::size_t Capacity, std::uint32_t MaxReadAttempts>
void run_stress() {
    static common::BroadcastQueue<Frame, Capacity, MaxReadAttempts> queue{};
    std::atomic<bool> done{false};
    std::atomic<std::size_t> subscribed{0U};
    std::array<ConsumerReport, kStressConsumers> reports{};
    std::vector<std::thread> consumers{};
    for (std::size_t i{0U}; i < kStressConsumers; ++i) {
        consumers.emplace_back([&done, &subscribed, &reports, i]() {
            consume(queue, done, subscribed, kConsumerWorkSpins[i], reports[i]);
        });
    }
    // Publish only once every consumer has subscribed, so that each one starts at message 0.
    while (subscribed.load(std::memory_order_acquire) < kStressConsumers) {
        std::this_thread::yield();
    }
    for (std::uint64_t sequence{0U}; sequence < kStressMessages; ++sequence) {
        queue.publish(make_frame(sequence));
        if (((sequence / kStressStretch) % 2U) == 1U) {
            for (std::uint32_t spin{0U}; spin < kProducerPauseSpins; ++spin) {
                common::cpu_relax();
            }
        }
    }
    done.store(true, std::memory_order_release);
    for (std::thread& consumer : consumers) {
        consumer.join();
    }

    CHECK(queue.published() == kStressMessages);
    for (const ConsumerReport& report : reports) {
        CHECK(report.errors == 0U);
        CHECK(report.start == 0U);
        CHECK((report.received + report.lost) == kStressMessages);  // Exact loss accounting.
        CHECK(report.last == (kStressMessages - 1U));               // The final drain reaches the newest message.
    }
}

}  // namespace

TEST_CASE(broadcast_queue_subscribe_sees_only_later_messages) {
    static common::BroadcastQueue<Message, 8U> queue{};
    publish_range(queue, 0U, 3U);
    CHECK(queue.published() == 3U);

    common::BroadcastCursor cursor{queue.subscribe()};
    CHECK(cursor.next == 3U);
    CHECK(reads_empty(queue, cursor));  // Messages 0 to 2 were published before subscribing.

    publish_range(queue, 3U, 2U);
    CHECK(reads(queue, cursor, 3U, 0U));
    CHECK(reads(queue, cursor, 4U, 0U));
    CHECK(reads_empty(queue, cursor));
    CHECK(reads_empty(queue, cursor));  // Stays empty, and the cursor stays where it is.
    CHECK(queue.published() == 5U);
}

TEST_CASE(broadcast_queue_cursor_before_first_publish) {
    static common::BroadcastQueue<Message, 4U> queue{};
    CHECK(queue.published() == 0U);
    common::BroadcastCursor early{queue.subscribe()};
    common::BroadcastCursor from_start{};  // Value-initialised: also starts at message 0.
    CHECK(early.next == 0U);
    CHECK(reads_empty(queue, early));
    CHECK(reads_empty(queue, from_start));

    queue.publish(make_message(0U));
    CHECK(reads(queue, early, 0U, 0U));
    CHECK(reads_empty(queue, early));

    publish_range(queue, 1U, 3U);  // Fills the four slots; nothing is overwritten yet.
    CHECK(reads(queue, early, 1U, 0U));
    for (std::uint64_t sequence{0U}; sequence < 4U; ++sequence) {
        CHECK(reads(queue, from_start, sequence, 0U));
    }
    CHECK(reads_empty(queue, from_start));

    // Messages 4 and 5 replace 0 and 1: a value-initialised cursor created now reports them lost.
    publish_range(queue, 4U, 2U);
    common::BroadcastCursor late{};
    CHECK(reads(queue, late, 2U, 2U));
    const Tally rest{drain(queue, late)};
    CHECK(rest.consistent);
    CHECK(rest.received == 3U);
    CHECK(rest.lost == 0U);
}

TEST_CASE(broadcast_queue_wraparound) {
    // Batches of 1 to 4 messages through a 4-slot queue, 250 laps in all. After a batch of 4 the
    // consumer is exactly Capacity behind, which still loses nothing.
    static common::BroadcastQueue<Message, 4U> queue{};
    common::BroadcastCursor cursor{queue.subscribe()};
    bool in_order{true};
    std::uint64_t next{0U};
    for (std::uint64_t batch{0U}; batch < 400U; ++batch) {
        const std::uint64_t count{(batch % 4U) + 1U};
        publish_range(queue, next, count);
        for (std::uint64_t sequence{next}; sequence < (next + count); ++sequence) {
            in_order = in_order && reads(queue, cursor, sequence, 0U);
        }
        in_order = in_order && reads_empty(queue, cursor);
        next += count;
    }
    CHECK(in_order);
    CHECK(next == 1'000U);
    CHECK(queue.published() == next);
    CHECK(cursor.next == next);
}

TEST_CASE(broadcast_queue_overrun_reports_exact_lost_counts) {
    using Queue = common::BroadcastQueue<Message, 8U>;
    static Queue queue{};
    CHECK(Queue::capacity() == 8U);

    // Exactly Capacity behind: the oldest message is still in its slot, so nothing is lost.
    common::BroadcastCursor exact{queue.subscribe()};
    publish_range(queue, 0U, 8U);
    for (std::uint64_t sequence{0U}; sequence < 8U; ++sequence) {
        CHECK(reads(queue, exact, sequence, 0U));
    }
    CHECK(reads_empty(queue, exact));

    // Capacity + 1 behind: message 16 has replaced message 8.
    common::BroadcastCursor one_over{queue.subscribe()};
    CHECK(one_over.next == 8U);
    publish_range(queue, 8U, 9U);
    CHECK(reads(queue, one_over, 9U, 1U));
    for (std::uint64_t sequence{10U}; sequence < 17U; ++sequence) {
        CHECK(reads(queue, one_over, sequence, 0U));
    }
    CHECK(reads_empty(queue, one_over));

    // Exactly 2 * Capacity behind: the cursor's slot holds the complete message 25, the oldest left.
    common::BroadcastCursor two_laps{queue.subscribe()};
    CHECK(two_laps.next == 17U);
    publish_range(queue, 17U, 16U);
    CHECK(reads(queue, two_laps, 25U, 8U));

    // Several laps behind: 37 messages behind, so the 8 newest are left and 29 are lost.
    common::BroadcastCursor far{queue.subscribe()};
    CHECK(far.next == 33U);
    publish_range(queue, 33U, 37U);
    CHECK(queue.published() == 70U);
    CHECK(reads(queue, far, 62U, 29U));

    // Every cursor is behind again. Once each has caught up: received + lost == published since it
    // subscribed.
    const Tally exact_rest{drain(queue, exact)};
    CHECK(exact_rest.consistent);
    CHECK((exact_rest.received == 8U) && (exact_rest.lost == 54U));
    CHECK((8U + exact_rest.received + exact_rest.lost) == 70U);

    const Tally one_over_rest{drain(queue, one_over)};
    CHECK(one_over_rest.consistent);
    CHECK((one_over_rest.received == 8U) && (one_over_rest.lost == 45U));
    CHECK((8U + 1U + one_over_rest.received + one_over_rest.lost) == (70U - 8U));

    const Tally two_laps_rest{drain(queue, two_laps)};
    CHECK(two_laps_rest.consistent);
    CHECK((two_laps_rest.received == 8U) && (two_laps_rest.lost == 36U));
    CHECK((1U + 8U + two_laps_rest.received + two_laps_rest.lost) == (70U - 17U));

    const Tally far_rest{drain(queue, far)};
    CHECK(far_rest.consistent);
    CHECK((far_rest.received == 7U) && (far_rest.lost == 0U));
    CHECK((1U + 29U + far_rest.received + far_rest.lost) == (70U - 33U));
}

TEST_CASE(broadcast_queue_minimum_capacity_and_attempts) {
    // Two slots and two attempts: an overrun cursor still recovers in one call when the producer
    // is idle (one attempt finds the overrun, the second reads the oldest message).
    static common::BroadcastQueue<Message, 2U, 2U> queue{};
    common::BroadcastCursor cursor{queue.subscribe()};
    publish_range(queue, 0U, 5U);
    CHECK(reads(queue, cursor, 3U, 3U));
    CHECK(reads(queue, cursor, 4U, 0U));
    CHECK(reads_empty(queue, cursor));

    publish_range(queue, 5U, 2U);  // Exactly Capacity behind.
    CHECK(reads(queue, cursor, 5U, 0U));
    CHECK(reads(queue, cursor, 6U, 0U));

    publish_range(queue, 7U, 4U);  // Exactly 2 * Capacity behind.
    CHECK(reads(queue, cursor, 9U, 2U));
    CHECK(reads(queue, cursor, 10U, 0U));
    CHECK(reads_empty(queue, cursor));
}

TEST_CASE(broadcast_queue_payload_layouts) {
    static common::BroadcastQueue<std::uint16_t, 4U> narrow{};
    common::BroadcastCursor narrow_cursor{narrow.subscribe()};
    narrow.publish(std::uint16_t{0xBEEFU});
    std::uint16_t narrow_out{0U};
    CHECK(narrow.try_read(narrow_cursor, narrow_out).status == ReadStatus::kOk);
    CHECK(narrow_out == std::uint16_t{0xBEEFU});

    static common::BroadcastQueue<Packed12, 4U> packed{};
    common::BroadcastCursor packed_cursor{packed.subscribe()};
    packed.publish(Packed12{1U, 0xFFFF'FFFFU, 3U});
    Packed12 packed_out{};
    CHECK(packed.try_read(packed_cursor, packed_out).status == ReadStatus::kOk);
    CHECK((packed_out.a == 1U) && (packed_out.b == 0xFFFF'FFFFU) && (packed_out.c == 3U));

    static common::BroadcastQueue<Wide, 4U> wide{};
    common::BroadcastCursor wide_cursor{wide.subscribe()};
    Wide wide_in{};
    for (std::size_t i{0U}; i < wide_in.words.size(); ++i) {
        wide_in.words[i] = (i * 0x0101'0101'0101'0101U) + 7U;
    }
    wide.publish(wide_in);
    Wide wide_out{};
    CHECK(wide.try_read(wide_cursor, wide_out).status == ReadStatus::kOk);
    CHECK(wide_out.words == wide_in.words);

    static common::BroadcastQueue<Quote, 4U> quotes{};
    common::BroadcastCursor quote_cursor{quotes.subscribe()};
    const Quote quote_in{101.25, -7, 0x5U};
    quotes.publish(quote_in);
    Quote quote_out{};
    CHECK(quotes.try_read(quote_cursor, quote_out).status == ReadStatus::kOk);
    CHECK(std::memcmp(&quote_out, &quote_in, sizeof(Quote)) == 0);  // Bit-exact, without == on doubles.
}

TEST_CASE(broadcast_queue_matches_reference_model) {
    // Pseudo-random publishes, reads and re-subscriptions by four consumers, each read checked
    // against the model.
    constexpr std::size_t kCapacity{16U};
    constexpr std::size_t kConsumers{4U};
    static common::BroadcastQueue<Message, kCapacity> queue{};
    std::array<common::BroadcastCursor, kConsumers> cursors{};
    std::array<ModelConsumer, kConsumers> models{};
    std::uint64_t published{0U};
    std::uint64_t state{0x2545'F491'4F6C'DD1DU};
    std::uint32_t mismatches{0U};
    for (std::uint32_t step{0U}; step < 20'000U; ++step) {
        state = (state * 6'364'136'223'846'793'005U) + 1'442'695'040'888'963'407U;
        const std::uint64_t random{state >> 33U};
        const std::size_t who{random % kConsumers};
        const std::uint64_t amount{(random / 16U) % ((2U * kCapacity) + 4U)};  // 0 to 2 * Capacity + 3
        const std::uint64_t action{(random / 4'096U) % 8U};
        if (action < 3U) {
            publish_range(queue, published, amount);
            published += amount;
        } else if (action == 3U) {
            cursors[who] = queue.subscribe();
            mismatches += (cursors[who].next == published) ? 0U : 1U;
            models[who] = ModelConsumer{published, published, Tally{}};
        } else {
            for (std::uint64_t i{0U}; i < (amount % 6U); ++i) {
                Message out{};
                const common::BroadcastRead read{queue.try_read(cursors[who], out)};
                const bool matches{
                    check_against_model(models[who], read, out, cursors[who].next, published, kCapacity)};
                mismatches += matches ? 0U : 1U;
            }
        }
    }
    CHECK(mismatches == 0U);
    CHECK(queue.published() == published);

    // Once caught up, every consumer has received or lost each message published since it subscribed.
    for (std::size_t i{0U}; i < kConsumers; ++i) {
        const Tally rest{drain(queue, cursors[i])};
        CHECK(rest.consistent);
        CHECK(cursors[i].next == published);
        const std::uint64_t accounted{models[i].tally.received + models[i].tally.lost + rest.received + rest.lost};
        CHECK(accounted == (published - models[i].start));
    }
}

TEST_CASE(broadcast_queue_threaded_stress_small_capacity) {
    run_stress<16U, 16U>();  // Constant overruns, and copies overwritten while they are read.
}

TEST_CASE(broadcast_queue_threaded_stress_large_capacity) {
    run_stress<1'024U, 16U>();  // The slower consumers fall behind during the producer's bursts.
}

TEST_CASE(broadcast_queue_threaded_stress_retry_path) {
    // With two attempts, a consumer racing the producer often gets kRetry, which must change nothing.
    run_stress<16U, 2U>();
}
