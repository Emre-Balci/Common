#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "common/containers/intrusive_list.hpp"
#include "common/containers/object_pool.hpp"
#include "test_framework.hpp"

namespace {

struct ByExpiry final {};  // Tag of a second list.

class Order final : public common::IntrusiveListHook<Order>, public common::IntrusiveListHook<Order, ByExpiry> {
public:
    explicit Order(const std::uint32_t id) noexcept : id_{id} {}

    [[nodiscard]] std::uint32_t id() const noexcept {
        return id_;
    }

private:
    std::uint32_t id_;
};

using Level = common::IntrusiveList<Order>;
using Expiries = common::IntrusiveList<Order, ByExpiry>;

/// The ids from front to back, walking with next().
std::vector<std::uint32_t> ids_forward(Level& list) {
    std::vector<std::uint32_t> ids{};
    Order* current{list.front()};
    for (std::size_t i{0U}; (i < 100U) && (current != nullptr); ++i) {
        ids.push_back(current->id());
        current = list.next(*current);
    }
    return ids;
}

/// The ids from back to front, walking with prev().
std::vector<std::uint32_t> ids_backward(Level& list) {
    std::vector<std::uint32_t> ids{};
    Order* current{list.back()};
    for (std::size_t i{0U}; (i < 100U) && (current != nullptr); ++i) {
        ids.insert(ids.begin(), current->id());
        current = list.prev(*current);
    }
    return ids;
}

}  // namespace

TEST_CASE(intrusive_list_insert_and_remove) {
    Order a{1U};
    Order b{2U};
    Order c{3U};
    Order d{4U};
    Level list{};
    CHECK(list.empty());
    CHECK(list.front() == nullptr);
    CHECK(list.back() == nullptr);
    CHECK(list.pop_front() == nullptr);
    CHECK(list.pop_back() == nullptr);

    list.push_back(b);
    list.push_front(a);
    list.push_back(d);
    list.insert_before(d, c);
    CHECK(list.size() == 4U);
    CHECK((ids_forward(list) == std::vector<std::uint32_t>{1U, 2U, 3U, 4U}));
    CHECK((ids_backward(list) == std::vector<std::uint32_t>{1U, 2U, 3U, 4U}));
    CHECK(list.contains(c));
    CHECK(Level::is_linked(c));
    CHECK(c.common::IntrusiveListHook<Order>::is_linked());
    CHECK(list.next(d) == nullptr);
    CHECK(list.prev(a) == nullptr);

    list.remove(b);  // Middle.
    CHECK(!list.contains(b));
    CHECK((ids_forward(list) == std::vector<std::uint32_t>{1U, 3U, 4U}));
    list.insert_after(a, b);
    CHECK((ids_forward(list) == std::vector<std::uint32_t>{1U, 2U, 3U, 4U}));
    list.remove(a);  // Front.
    list.remove(d);  // Back.
    CHECK((ids_forward(list) == std::vector<std::uint32_t>{2U, 3U}));
    CHECK((ids_backward(list) == std::vector<std::uint32_t>{2U, 3U}));

    CHECK(list.pop_front() == &b);
    CHECK(list.pop_back() == &c);
    CHECK(list.empty());
    CHECK(!Level::is_linked(b));
}

TEST_CASE(intrusive_list_two_lists_at_once) {
    Order a{1U};
    Order b{2U};
    Level level{};
    Expiries expiries{};
    level.push_back(a);
    level.push_back(b);
    expiries.push_back(b);
    expiries.push_back(a);
    CHECK(level.front() == &a);
    CHECK(expiries.front() == &b);
    CHECK(level.contains(a));
    CHECK(expiries.contains(a));

    // Another list of the same tag does not contain the element.
    Level other{};
    CHECK(!other.contains(a));
    CHECK(Level::is_linked(a));

    level.remove(a);
    CHECK(!Level::is_linked(a));
    CHECK(Expiries::is_linked(a));
    expiries.clear();
    level.clear();
    CHECK(!Expiries::is_linked(a));
    CHECK(!Expiries::is_linked(b));
}

TEST_CASE(intrusive_list_copies_start_unlinked) {
    Order a{1U};
    Level list{};
    list.push_back(a);
    Order copy{a};  // A new element: not in the list.
    CHECK(!Level::is_linked(copy));
    CHECK(copy.id() == 1U);
    Order moved{std::move(copy)};
    CHECK(!Level::is_linked(moved));

    // Assignment copies the value, never the place in a list.
    Order b{2U};
    b = a;
    CHECK(b.id() == 1U);
    CHECK(!Level::is_linked(b));
    a = Order{3U};
    CHECK(list.contains(a));
    CHECK(a.id() == 3U);
    CHECK(list.size() == 1U);
    list.clear();
}

TEST_CASE(intrusive_list_for_each) {
    std::vector<Order> orders{};
    orders.reserve(6U);
    for (std::uint32_t id{1U}; id <= 6U; ++id) {
        orders.emplace_back(id);
    }
    Level list{};
    for (Order& order : orders) {
        list.push_back(order);
    }

    std::uint32_t sum{0U};
    const Level& view{list};
    view.for_each([&sum](const Order& order) noexcept { sum += order.id(); });
    CHECK(sum == 21U);

    // The visitor may remove the element it was given.
    list.for_each([&list](Order& order) noexcept {
        if ((order.id() % 2U) == 0U) {
            list.remove(order);
        }
    });
    CHECK((ids_forward(list) == std::vector<std::uint32_t>{1U, 3U, 5U}));

    // Returning false stops the walk.
    std::vector<std::uint32_t> visited{};
    list.for_each([&visited](Order& order) noexcept {
        visited.push_back(order.id());
        return order.id() < 3U;
    });
    CHECK((visited == std::vector<std::uint32_t>{1U, 3U}));
    list.clear();
    CHECK(list.empty());
}

TEST_CASE(intrusive_list_with_object_pool) {
    common::ObjectPool<Order, 8U> pool{};
    {
        Level level{};  // Declared after the pool, destroyed first.
        for (std::uint32_t id{0U}; id < 8U; ++id) {
            Order* const order{pool.create(id)};
            REQUIRE(order != nullptr);
            level.push_back(*order);
        }
        CHECK(level.size() == 8U);
        // FIFO: the oldest order is filled first.
        Order* const filled{level.pop_front()};
        REQUIRE(filled != nullptr);
        CHECK(filled->id() == 0U);
        pool.destroy(filled);
        // The level unlinks the rest when it is destroyed; the pool still owns them.
    }
    CHECK(pool.in_use() == 7U);
}
