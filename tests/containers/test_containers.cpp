#include <cstdint>

#include "common/containers/object_pool.hpp"
#include "common/containers/ring_buffer.hpp"
#include "common/containers/static_vector.hpp"
#include "test_framework.hpp"

namespace {

/// Counts live instances to verify constructor / destructor pairing.
class Tracked final {
public:
    explicit Tracked(const std::int32_t value) noexcept : value_{value} {
        ++live_;
    }
    Tracked(const Tracked& other) noexcept : value_{other.value_} {
        ++live_;
    }
    Tracked(Tracked&& other) noexcept : value_{other.value_} {
        other.value_ = -1;
        ++live_;
    }
    Tracked& operator=(const Tracked& other) noexcept = default;
    Tracked& operator=(Tracked&& other) noexcept {
        value_ = other.value_;
        other.value_ = -1;
        return *this;
    }
    ~Tracked() {
        --live_;
    }

    [[nodiscard]] std::int32_t value() const noexcept {
        return value_;
    }
    static std::int32_t live() noexcept {
        return live_;
    }

private:
    std::int32_t value_;
    static inline std::int32_t live_{0};
};

}  // namespace

TEST_CASE(static_vector_capacity_and_lifetime) {
    {
        common::StaticVector<Tracked, 3U> vec{};
        CHECK(vec.empty());
        CHECK(vec.emplace_back(1));
        CHECK(vec.emplace_back(2));
        CHECK(vec.push_back(Tracked{3}));
        CHECK(vec.full());
        CHECK(!vec.emplace_back(4));
        CHECK(Tracked::live() == 3);
        CHECK(vec[0U].value() == 1);
        CHECK(vec.back().value() == 3);

        std::int32_t sum{0};
        for (const Tracked& t : vec) {
            sum += t.value();
        }
        CHECK(sum == 6);

        common::StaticVector<Tracked, 3U> copy{vec};
        CHECK(copy.size() == 3U);
        CHECK(Tracked::live() == 6);

        vec.erase(0U);  // Ordered: [2, 3]
        CHECK(vec.size() == 2U);
        CHECK(vec[0U].value() == 2);
        CHECK(vec[1U].value() == 3);

        copy.erase_unordered(0U);  // Unordered: [3, 2]
        CHECK(copy[0U].value() == 3);
        CHECK(copy[1U].value() == 2);

        common::StaticVector<Tracked, 3U> moved{std::move(copy)};
        CHECK(moved.size() == 2U);
        CHECK(copy.empty());
        CHECK(Tracked::live() == 4);
    }
    CHECK(Tracked::live() == 0);
}

TEST_CASE(object_pool_create_destroy_reuse) {
    {
        common::ObjectPool<Tracked, 2U> pool{};
        Tracked* const a{pool.create(10)};
        Tracked* const b{pool.create(20)};
        REQUIRE((a != nullptr) && (b != nullptr));
        CHECK(pool.create(30) == nullptr);
        CHECK(pool.available() == 0U);
        CHECK(pool.owns(a));
        CHECK(pool.owns(b));

        const Tracked outside{0};
        CHECK(!pool.owns(&outside));

        pool.destroy(a);
        CHECK(!pool.owns(a));
        CHECK(pool.available() == 1U);
        Tracked* const c{pool.create(40)};
        CHECK(c == a);  // Freed slot is reused.
        CHECK((c != nullptr) && (c->value() == 40));
        pool.destroy(nullptr);
        CHECK(Tracked::live() == 3);
    }
    CHECK(Tracked::live() == 0);  // Pool destructor released live objects.
}

TEST_CASE(ring_buffer_overwrite_window) {
    common::RingBuffer<std::int32_t, 3U> ring{};
    CHECK(!ring.push_overwrite(1));
    CHECK(!ring.push_overwrite(2));
    CHECK(!ring.push_overwrite(3));
    CHECK(ring.full());
    CHECK(!ring.try_push(4));
    CHECK(ring.push_overwrite(4));  // Drops 1.
    CHECK(ring.front() == 2);
    CHECK(ring.back() == 4);
    CHECK(ring[1U] == 3);

    std::int32_t out{0};
    REQUIRE(ring.try_pop_front(out));
    CHECK(out == 2);
    CHECK(ring.try_push(5));
    CHECK(ring.size() == 3U);
    CHECK(ring[0U] == 3);
    CHECK(ring[2U] == 5);
    ring.clear();
    CHECK(ring.empty());
    CHECK(!ring.try_pop_front(out));
}
