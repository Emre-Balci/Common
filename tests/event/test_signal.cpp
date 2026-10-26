#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "common/core/status.hpp"
#include "common/event/signal.hpp"
#include "common/functional/inplace_function.hpp"
#include "test_framework.hpp"

namespace {

/// Records the order in which slots run.
struct Log final {
    std::array<std::int32_t, 32U> entries{};
    std::size_t size{0U};

    void add(const std::int32_t value) noexcept {
        if (size < entries.size()) {
            entries[size] = value;
            ++size;
        }
    }
};

/// Counts its live copies, to check that a slot's callable is destroyed exactly once and never while
/// it runs.
class Tracked final {
public:
    Tracked(std::int32_t& live, std::int32_t& calls) noexcept : live_{&live}, calls_{&calls} {
        ++*live_;
    }
    Tracked(const Tracked& other) noexcept : live_{other.live_}, calls_{other.calls_} {
        ++*live_;
    }
    Tracked(Tracked&& other) noexcept : live_{other.live_}, calls_{other.calls_} {
        ++*live_;
    }
    Tracked& operator=(const Tracked&) = delete;
    Tracked& operator=(Tracked&&) = delete;
    ~Tracked() {
        --*live_;
    }
    void operator()(std::int32_t /*value*/) const noexcept {
        ++*calls_;
    }

private:
    std::int32_t* live_;
    std::int32_t* calls_;
};

}  // namespace

TEST_CASE(signal_connect_notify_disconnect) {
    common::Signal<void(std::int32_t), 4U> signal{};
    CHECK(signal.empty());
    CHECK(signal.capacity() == 4U);
    CHECK(signal.notify(1) == 0U);

    std::int32_t total{0};
    const common::Result<common::Connection> first{signal.connect([&total](const std::int32_t value) noexcept {
        total += value;
    })};
    REQUIRE(first.has_value());
    const common::Result<common::Connection> second{signal.connect([&total](const std::int32_t value) noexcept {
        total += 10 * value;
    })};
    REQUIRE(second.has_value());
    CHECK(signal.size() == 2U);
    CHECK(signal.connected(first.value()));

    CHECK(signal.notify(2) == 2U);
    CHECK(total == 22);

    CHECK(signal.disconnect(first.value()) == common::Status::kOk);
    CHECK(!signal.connected(first.value()));
    CHECK(signal.disconnect(first.value()) == common::Status::kNotFound);
    CHECK(signal.disconnect(common::Connection{}) == common::Status::kNotFound);
    CHECK(signal.notify(1) == 1U);
    CHECK(total == 32);

    signal.disconnect_all();
    CHECK(signal.empty());
    CHECK(signal.notify(1) == 0U);
}

TEST_CASE(signal_calls_in_connection_order) {
    common::Signal<void(), 4U> signal{};
    Log log{};
    const common::Result<common::Connection> a{signal.connect([&log]() noexcept { log.add(1); })};
    const common::Result<common::Connection> b{signal.connect([&log]() noexcept { log.add(2); })};
    REQUIRE(a.has_value() && b.has_value());
    // c reuses a's entry (index 0) but is called after b.
    CHECK(signal.disconnect(a.value()) == common::Status::kOk);
    const common::Result<common::Connection> c{signal.connect([&log]() noexcept { log.add(3); })};
    REQUIRE(c.has_value());
    CHECK(c.value().index == a.value().index);
    CHECK(c.value() != a.value());  // Another generation: a stays stale.
    CHECK(!signal.connected(a.value()));
    signal.notify();
    REQUIRE(log.size == 2U);
    CHECK(log.entries[0U] == 2);
    CHECK(log.entries[1U] == 3);
}

TEST_CASE(signal_capacity_and_empty_callables) {
    common::Signal<void(), 2U> signal{};
    CHECK(signal.connect([]() noexcept {}).has_value());
    CHECK(signal.connect([]() noexcept {}).has_value());
    CHECK(signal.connect([]() noexcept {}).status() == common::Status::kResourceLimit);

    common::Signal<void(), 2U> other{};
    using Function = void (*)() noexcept;
    const Function none{nullptr};
    CHECK(other.connect(none).status() == common::Status::kInvalidArgument);
    const common::InplaceFunction<void(), 8U> empty_small{};
    CHECK(other.connect(empty_small).status() == common::Status::kInvalidArgument);
    CHECK(other.empty());
}

TEST_CASE(signal_slot_disconnects_itself_and_others) {
    using IntSignal = common::Signal<void(std::int32_t), 4U>;
    IntSignal signal{};
    Log log{};
    common::Connection self{};
    common::Connection later{};
    // The first slot disconnects itself and the third; the third is not called in this notification.
    const common::Result<common::Connection> first{signal.connect([&](const std::int32_t value) noexcept {
        log.add(value);
        CHECK(signal.disconnect(self) == common::Status::kOk);
        CHECK(signal.disconnect(later) == common::Status::kOk);
        CHECK(!signal.connected(later));
        CHECK(signal.size() == 1U);  // Takes effect at once.
    })};
    REQUIRE(first.has_value());
    self = first.value();
    const common::Result<common::Connection> second{signal.connect([&log](const std::int32_t value) noexcept {
        log.add(100 + value);
    })};
    const common::Result<common::Connection> third{signal.connect([&log](const std::int32_t value) noexcept {
        log.add(200 + value);
    })};
    REQUIRE(second.has_value() && third.has_value());
    later = third.value();

    CHECK(signal.notify(1) == 2U);
    REQUIRE(log.size == 2U);
    CHECK(log.entries[0U] == 1);
    CHECK(log.entries[1U] == 101);

    CHECK(signal.notify(2) == 1U);
    CHECK(log.size == 3U);
    CHECK(log.entries[2U] == 102);
}

TEST_CASE(signal_connect_during_notification) {
    common::Signal<void(), 3U> signal{};
    Log log{};
    bool connected_once{false};
    const common::Result<common::Connection> first{signal.connect([&]() noexcept {
        log.add(1);
        if (!connected_once) {
            connected_once = true;
            CHECK(signal.connect([&log]() noexcept { log.add(2); }).has_value());
        }
    })};
    REQUIRE(first.has_value());
    CHECK(signal.notify() == 1U);  // The new slot waits for the next notification.
    CHECK(log.size == 1U);
    CHECK(signal.size() == 2U);
    CHECK(signal.notify() == 2U);
    REQUIRE(log.size == 3U);
    CHECK(log.entries[1U] == 1);
    CHECK(log.entries[2U] == 2);
}

TEST_CASE(signal_released_entries_are_freed_after_the_notification) {
    common::Signal<void(), 2U> signal{};
    common::Connection self{};
    common::Status inside{common::Status::kOk};
    const common::Result<common::Connection> first{signal.connect([&]() noexcept {
        CHECK(signal.disconnect(self) == common::Status::kOk);
        // The released entry is still in use until the notification ends: the only other entry is
        // taken by the second slot, so there is no room yet.
        inside = signal.connect([]() noexcept {}).status();
    })};
    REQUIRE(first.has_value());
    self = first.value();
    REQUIRE(signal.connect([]() noexcept {}).has_value());
    signal.notify();
    CHECK(inside == common::Status::kResourceLimit);
    CHECK(signal.size() == 1U);
    CHECK(signal.connect([]() noexcept {}).has_value());  // Freed now.
}

TEST_CASE(signal_destroys_each_callable_once_and_not_while_running) {
    std::int32_t live{0};
    std::int32_t calls{0};
    {
        common::Signal<void(std::int32_t), 2U> signal{};
        common::Connection self{};
        const Tracked tracked{live, calls};
        REQUIRE(signal.connect(tracked).has_value());
        CHECK(live == 2);  // The local and the stored copy.
        const common::Result<common::Connection> disconnecting{signal.connect([&](std::int32_t) noexcept {
            const std::int32_t before{live};
            CHECK(signal.disconnect(self) == common::Status::kOk);
            CHECK(live == before);  // Not destroyed while the notification runs.
        })};
        REQUIRE(disconnecting.has_value());
        self = disconnecting.value();
        signal.notify(1);
        CHECK(calls == 1);
        signal.disconnect_all();
        CHECK(live == 1);
    }
    CHECK(live == 0);
}

TEST_CASE(signal_arguments_by_value_and_const_reference) {
    struct Quote final {
        std::int64_t price{0};
        std::int64_t quantity{0};
    };
    common::Signal<void(const Quote&, std::int32_t), 2U> signal{};
    std::int64_t notional{0};
    std::int32_t last_venue{0};
    REQUIRE(signal.connect([&](const Quote& quote, const std::int32_t venue) noexcept {
        notional += quote.price * quote.quantity;
        last_venue = venue;
    }).has_value());
    REQUIRE(signal.connect([&notional](const Quote& quote, std::int32_t) noexcept { notional += quote.price; })
                .has_value());
    const Quote quote{100, 3};
    CHECK(signal.notify(quote, 7) == 2U);
    CHECK(notional == 400);
    CHECK(last_venue == 7);
}

TEST_CASE(signal_scoped_connection) {
    common::Signal<void(), 2U> signal{};
    std::int32_t calls{0};
    {
        const common::Result<common::Connection> connection{signal.connect([&calls]() noexcept { ++calls; })};
        REQUIRE(connection.has_value());
        const common::ScopedConnection scoped{signal, connection.value()};
        CHECK(scoped.connection() == connection.value());
        signal.notify();
        CHECK(calls == 1);
    }
    CHECK(signal.empty());  // Disconnected on destruction.

    // Move construction and assignment transfer the connection; reset() disconnects.
    const common::Result<common::Connection> a{signal.connect([&calls]() noexcept { ++calls; })};
    const common::Result<common::Connection> b{signal.connect([&calls]() noexcept { ++calls; })};
    REQUIRE(a.has_value() && b.has_value());
    common::ScopedConnection first{signal, a.value()};
    common::ScopedConnection moved{std::move(first)};
    CHECK(first.connection() == common::Connection{});  // NOLINT(bugprone-use-after-move): documented state.
    CHECK(moved.connection() == a.value());
    common::ScopedConnection second{signal, b.value()};
    moved = std::move(second);  // Disconnects a, takes over b.
    CHECK(!signal.connected(a.value()));
    CHECK(signal.connected(b.value()));
    moved.reset();
    CHECK(signal.empty());
    moved.reset();  // Nothing left to disconnect.

    // release() keeps the slot connected.
    const common::Result<common::Connection> c{signal.connect([&calls]() noexcept { ++calls; })};
    REQUIRE(c.has_value());
    {
        common::ScopedConnection scoped{signal, c.value()};
        CHECK(scoped.release() == c.value());
    }
    CHECK(signal.connected(c.value()));
}
