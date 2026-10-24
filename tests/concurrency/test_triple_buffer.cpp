#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

#include "common/concurrency/triple_buffer.hpp"
#include "common/containers/static_vector.hpp"
#include "test_framework.hpp"

namespace {

/// Larger than a cache line; every payload word equals sequence, so a torn copy is detectable.
struct Snapshot final {
    std::uint64_t sequence{0U};
    std::array<std::uint64_t, 15U> payload{};
};

Snapshot make_snapshot(const std::uint64_t sequence) noexcept {
    Snapshot snapshot{};
    snapshot.sequence = sequence;
    snapshot.payload.fill(sequence);
    return snapshot;
}

bool consistent(const Snapshot& snapshot) noexcept {
    bool ok{true};
    for (const std::uint64_t word : snapshot.payload) {
        ok = ok && (word == snapshot.sequence);
    }
    return ok;
}

common::TripleBuffer<Snapshot> g_stress_buffer{};

}  // namespace

TEST_CASE(triple_buffer_latest_value) {
    static common::TripleBuffer<Snapshot> buffer{};
    CHECK(!buffer.update());
    CHECK(buffer.read().sequence == 0U);  // Default value before the first publish.

    buffer.write(make_snapshot(1U));
    buffer.write(make_snapshot(2U));  // The reader skips 1 and gets the newest value.
    CHECK(buffer.update());
    CHECK(buffer.read().sequence == 2U);
    CHECK(!buffer.update());
    CHECK(buffer.read().sequence == 2U);  // Stays readable until the next update.

    Snapshot& slot{buffer.write_buffer()};
    slot = make_snapshot(3U);
    buffer.publish();
    CHECK(buffer.read().sequence == 2U);  // Not visible before update().
    CHECK(buffer.update());
    CHECK(buffer.read().sequence == 3U);
    CHECK(consistent(buffer.read()));

    // Many writes without a read: every slot rotates, and the reader still gets the last one.
    for (std::uint64_t i{4U}; i <= 10U; ++i) {
        buffer.write(make_snapshot(i));
    }
    CHECK(buffer.update());
    CHECK(buffer.read().sequence == 10U);
}

TEST_CASE(triple_buffer_non_trivial_type) {
    // SeqLock needs a trivially copyable T; TripleBuffer only a nothrow copy assignment.
    using Levels = common::StaticVector<std::int32_t, 4U>;
    static common::TripleBuffer<Levels> buffer{};
    Levels levels{};
    CHECK(levels.push_back(100));
    CHECK(levels.push_back(101));
    buffer.write(levels);
    CHECK(buffer.update());
    REQUIRE(buffer.read().size() == 2U);
    CHECK(buffer.read()[1U] == 101);
}

TEST_CASE(triple_buffer_threaded_consistency) {
    constexpr std::uint64_t kWrites{500'000U};
    std::atomic<bool> done{false};
    std::thread writer{[&done]() {
        for (std::uint64_t i{1U}; i <= kWrites; ++i) {
            g_stress_buffer.write(make_snapshot(i));
        }
        done.store(true, std::memory_order_release);
    }};

    std::uint32_t torn{0U};
    std::uint32_t out_of_order{0U};
    std::uint64_t last{0U};
    std::uint64_t updates{0U};
    bool finished{false};
    while (!finished) {
        // Read done first: an update afterwards is guaranteed to see the final value.
        finished = done.load(std::memory_order_acquire);
        if (g_stress_buffer.update()) {
            const Snapshot& snapshot{g_stress_buffer.read()};
            torn += consistent(snapshot) ? 0U : 1U;
            out_of_order += (snapshot.sequence > last) ? 0U : 1U;
            last = snapshot.sequence;
            ++updates;
        }
    }
    writer.join();
    CHECK(torn == 0U);
    CHECK(out_of_order == 0U);
    CHECK(last == kWrites);
    CHECK(updates > 0U);
}
