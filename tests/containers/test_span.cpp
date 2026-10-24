#include <array>
#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/containers/static_vector.hpp"
#include "test_framework.hpp"

namespace {

constexpr std::array<std::int32_t, 4U> kValues{10, 20, 30, 40};

// Spans over static data are usable in constant expressions.
constexpr common::Span<const std::int32_t> kView{kValues};
static_assert(kView.size() == 4U, "constexpr size");
static_assert(kView[2U] == 30, "constexpr element access");
static_assert(kView.subspan(1U, 2U).back() == 30, "constexpr subspan");

std::int32_t sum(const common::Span<const std::int32_t> values) noexcept {
    std::int32_t total{0};
    for (const std::int32_t value : values) {
        total += value;
    }
    return total;
}

}  // namespace

TEST_CASE(span_views_containers) {
    std::array<std::int32_t, 4U> values{1, 2, 3, 4};
    const common::Span<std::int32_t> view{values};
    CHECK(view.size() == 4U);
    CHECK(view.size_bytes() == (4U * sizeof(std::int32_t)));
    CHECK(!view.empty());
    view[1U] = 20;  // Writes go to the viewed array.
    CHECK(values[1U] == 20);
    CHECK(view.front() == 1);
    CHECK(view.back() == 4);
    CHECK(sum(view) == 28);  // Span<T> converts to Span<const T>.

    common::StaticVector<std::int32_t, 8U> vector{};
    CHECK(vector.push_back(5));
    CHECK(vector.push_back(6));
    const common::Span<const std::int32_t> vector_view{vector};
    CHECK(vector_view.size() == 2U);
    CHECK(vector_view[1U] == 6);

    // Deduction guides.
    const common::Span deduced{values};
    CHECK(deduced.data() == values.data());
    const common::Span from_pointer{values.data(), 2U};
    CHECK(from_pointer.size() == 2U);
}

TEST_CASE(span_sub_views) {
    const common::Span<const std::int32_t> view{kValues};
    CHECK(view.first(2U).size() == 2U);
    CHECK(view.first(2U).back() == 20);
    CHECK(view.last(1U).front() == 40);
    CHECK(view.subspan(1U, 2U)[0U] == 20);
    CHECK(view.subspan(1U).size() == 3U);
    CHECK(view.subspan(4U).empty());
    CHECK(view.first(0U).empty());

    CHECK(view.in_bounds(0U, 4U));
    CHECK(view.in_bounds(4U, 0U));
    CHECK(!view.in_bounds(3U, 2U));
    CHECK(!view.in_bounds(5U, 0U));
    // An attacker-controlled length must not wrap around the check.
    CHECK(!view.in_bounds(2U, static_cast<std::size_t>(-1)));
}

TEST_CASE(span_empty) {
    const common::Span<std::uint8_t> empty{};
    CHECK(empty.empty());
    CHECK(empty.begin() == empty.end());
    CHECK(empty.data() == nullptr);
    CHECK(empty.in_bounds(0U, 0U));
    std::size_t visited{0U};
    for (const std::uint8_t byte : empty) {
        static_cast<void>(byte);
        ++visited;
    }
    CHECK(visited == 0U);
}
