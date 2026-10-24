#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "common/concurrency/seqlock.hpp"
#include "test_framework.hpp"

namespace {

struct Quote final {
    std::uint64_t sequence;
    std::int64_t bid;
    std::int64_t ask;
    std::uint32_t bid_size;
    std::uint32_t ask_size;
};

}  // namespace

TEST_CASE(seqlock_store_load) {
    static common::SeqLock<Quote> lock{};
    Quote out{};
    REQUIRE(lock.try_load(out));
    CHECK(out.sequence == 0U);
    const std::size_t initial_version{lock.version()};
    lock.store(Quote{7U, 100, 101, 5U, 6U});
    CHECK(lock.version() == (initial_version + 1U));
    REQUIRE(lock.try_load(out));
    CHECK(out.sequence == 7U);
    CHECK(out.bid == 100);
    CHECK(out.ask == 101);
    CHECK(out.ask_size == 6U);
}

TEST_CASE(seqlock_readers_never_see_torn_values) {
    static common::SeqLock<Quote, 1024U> lock{};
    constexpr std::uint64_t kWrites{500'000U};
    std::atomic<bool> done{false};
    std::atomic<std::uint32_t> torn{0U};

    std::vector<std::thread> readers{};
    for (std::uint32_t r{0U}; r < 3U; ++r) {
        readers.emplace_back([&done, &torn]() {
            Quote q{};
            std::uint64_t last{0U};
            while (!done.load(std::memory_order_acquire)) {
                // sequence 0 is the default-constructed initial value, which has no invariant.
                if (lock.try_load(q) && (q.sequence != 0U)) {
                    const auto s{static_cast<std::int64_t>(q.sequence)};
                    const bool consistent{(q.bid == s) && (q.ask == (s + 1)) &&
                                          (q.bid_size == static_cast<std::uint32_t>(q.sequence)) &&
                                          (q.sequence >= last)};
                    if (!consistent) {
                        torn.fetch_add(1U, std::memory_order_relaxed);
                    }
                    last = q.sequence;
                }
            }
        });
    }
    for (std::uint64_t i{1U}; i <= kWrites; ++i) {
        const auto s{static_cast<std::int64_t>(i)};
        lock.store(Quote{i, s, s + 1, static_cast<std::uint32_t>(i), 0U});
    }
    done.store(true, std::memory_order_release);
    for (std::thread& reader : readers) {
        reader.join();
    }
    CHECK(torn.load() == 0U);
}
