#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/app/unit.hpp"
#include "common/app/unit_manager.hpp"
#include "common/core/status.hpp"
#include "test_framework.hpp"

namespace {

enum class Hook : std::int32_t { kInitialize = 1, kStart = 2, kStop = 3, kShutdown = 4 };

/// The hook calls of all units, in order, as (unit id, hook).
struct HookLog final {
    std::array<std::int32_t, 64U> entries{};
    std::size_t size{0U};

    void add(const std::int32_t id, const Hook hook) noexcept {
        if (size < entries.size()) {
            entries[size] = (10 * id) + static_cast<std::int32_t>(hook);
            ++size;
        }
    }

    /// @return true if the log holds exactly @p expected.
    template <std::size_t N>
    [[nodiscard]] bool equals(const std::array<std::int32_t, N>& expected) const noexcept {
        bool same{size == N};
        for (std::size_t i{0U}; same && (i < N); ++i) {
            same = (entries[i] == expected[i]);
        }
        return same;
    }

    void clear() noexcept {
        size = 0U;
    }
};

class LoggingUnit final : public common::Unit {
public:
    LoggingUnit(const std::string_view name, const std::int32_t id, HookLog& log) noexcept
        : Unit{name}, id_{id}, log_{&log} {}

    common::Status initialize_result{common::Status::kOk};
    common::Status start_result{common::Status::kOk};

private:
    common::Status on_initialize() noexcept override {
        log_->add(id_, Hook::kInitialize);
        return initialize_result;
    }
    common::Status on_start() noexcept override {
        log_->add(id_, Hook::kStart);
        return start_result;
    }
    void on_stop() noexcept override {
        log_->add(id_, Hook::kStop);
    }
    void on_shutdown() noexcept override {
        log_->add(id_, Hook::kShutdown);
    }

    std::int32_t id_;
    HookLog* log_;
};

}  // namespace

TEST_CASE(unit_manager_orders_by_dependencies) {
    // e depends on nothing; a on b; b on c and d (a diamond through d); c on d.
    HookLog log{};
    LoggingUnit a{"a", 1, log};
    LoggingUnit b{"b", 2, log};
    LoggingUnit c{"c", 3, log};
    LoggingUnit d{"d", 4, log};
    LoggingUnit e{"e", 5, log};
    REQUIRE(a.depends_on(b) == common::Status::kOk);
    REQUIRE(b.depends_on(c) == common::Status::kOk);
    REQUIRE(b.depends_on(d) == common::Status::kOk);
    REQUIRE(c.depends_on(d) == common::Status::kOk);
    common::UnitManager<8U> manager{};
    for (LoggingUnit* unit : std::array<LoggingUnit*, 5U>{{&a, &b, &c, &d, &e}}) {
        REQUIRE(manager.add(*unit) == common::Status::kOk);
    }
    CHECK(&manager[0U] == &a);  // add() order until the dependency order is known.

    CHECK(manager.validate() == common::Status::kOk);
    // d first (no dependency), then c, b, a; e is ready from the start, but among ready units the
    // earliest added goes first, so it comes last.
    CHECK(&manager[0U] == &d);
    CHECK(&manager[4U] == &e);
    CHECK(manager.initialize() == common::Status::kOk);
    CHECK(log.equals(std::array<std::int32_t, 5U>{{41, 31, 21, 11, 51}}));
    CHECK(manager.failed_unit() == nullptr);

    log.clear();
    CHECK(manager.start() == common::Status::kOk);
    CHECK(log.equals(std::array<std::int32_t, 5U>{{42, 32, 22, 12, 52}}));

    log.clear();
    manager.stop();  // Reverse order.
    CHECK(log.equals(std::array<std::int32_t, 5U>{{53, 13, 23, 33, 43}}));
    CHECK(a.state() == common::UnitState::kInitialized);

    log.clear();
    manager.shutdown();
    CHECK(log.equals(std::array<std::int32_t, 5U>{{54, 14, 24, 34, 44}}));
    CHECK(d.state() == common::UnitState::kCreated);

    // Initialised again after the shutdown.
    log.clear();
    CHECK(manager.initialize() == common::Status::kOk);
    CHECK(log.size == 5U);
    manager.shutdown();
}

TEST_CASE(unit_manager_wiring_errors_run_no_hook) {
    HookLog log{};
    LoggingUnit a{"a", 1, log};
    LoggingUnit b{"b", 2, log};
    LoggingUnit outsider{"outsider", 3, log};
    {
        REQUIRE(a.depends_on(outsider) == common::Status::kOk);  // Not added to the manager.
        common::UnitManager<4U> manager{};
        REQUIRE(manager.add(a) == common::Status::kOk);
        REQUIRE(manager.add(b) == common::Status::kOk);
        CHECK(manager.validate() == common::Status::kNotFound);
        CHECK(manager.failed_unit() == &a);
        CHECK(manager.initialize() == common::Status::kNotFound);
        CHECK(manager.start() == common::Status::kNotFound);
        CHECK(log.size == 0U);
    }
    {
        LoggingUnit x{"x", 4, log};
        LoggingUnit y{"y", 5, log};
        REQUIRE(x.depends_on(y) == common::Status::kOk);
        REQUIRE(y.depends_on(x) == common::Status::kOk);  // A cycle.
        common::UnitManager<4U> manager{};
        REQUIRE(manager.add(x) == common::Status::kOk);
        REQUIRE(manager.add(y) == common::Status::kOk);
        CHECK(manager.initialize() == common::Status::kInvalidArgument);
        CHECK(manager.failed_unit() == &x);
        CHECK(log.size == 0U);
    }
    {
        LoggingUnit ready{"ready", 6, log};
        REQUIRE(ready.initialize() == common::Status::kOk);  // Initialised on its own.
        log.clear();
        common::UnitManager<2U> manager{};
        REQUIRE(manager.add(ready) == common::Status::kOk);
        CHECK(manager.validate() == common::Status::kInvalidArgument);  // Not kCreated.
        CHECK(manager.failed_unit() == &ready);
        CHECK(manager.start() == common::Status::kOk);  // But it can be started.
        manager.shutdown();
    }
}

TEST_CASE(unit_manager_rolls_back_a_failed_initialize) {
    HookLog log{};
    LoggingUnit a{"a", 1, log};
    LoggingUnit b{"b", 2, log};
    LoggingUnit c{"c", 3, log};
    LoggingUnit d{"d", 4, log};
    c.initialize_result = common::Status::kNotFound;
    common::UnitManager<4U> manager{};
    for (LoggingUnit* unit : std::array<LoggingUnit*, 4U>{{&a, &b, &c, &d}}) {
        REQUIRE(manager.add(*unit) == common::Status::kOk);
    }
    CHECK(manager.initialize() == common::Status::kNotFound);
    CHECK(manager.failed_unit() == &c);
    // a and b were initialised, c failed, d was never touched; b and a are shut down again.
    CHECK(log.equals(std::array<std::int32_t, 5U>{{11, 21, 31, 24, 14}}));
    CHECK(a.state() == common::UnitState::kCreated);
    CHECK(c.state() == common::UnitState::kCreated);

    // Fixed: it works the second time, and adding is still possible in between.
    c.initialize_result = common::Status::kOk;
    LoggingUnit e{"e", 5, log};
    CHECK(manager.add(e) == common::Status::kResourceLimit);
    CHECK(manager.initialize() == common::Status::kOk);
    CHECK(manager.add(e) == common::Status::kInvalidArgument);  // After initialize().
    manager.shutdown();
}

TEST_CASE(unit_manager_rolls_back_a_failed_start) {
    HookLog log{};
    LoggingUnit a{"a", 1, log};
    LoggingUnit b{"b", 2, log};
    LoggingUnit c{"c", 3, log};
    b.start_result = common::Status::kDisconnected;
    common::UnitManager<4U> manager{};
    REQUIRE(manager.add(a) == common::Status::kOk);
    REQUIRE(manager.add(b) == common::Status::kOk);
    REQUIRE(manager.add(c) == common::Status::kOk);
    CHECK(manager.start() == common::Status::kInvalidArgument);  // Not initialised.
    CHECK(manager.failed_unit() == &a);
    REQUIRE(manager.initialize() == common::Status::kOk);
    log.clear();
    CHECK(manager.start() == common::Status::kDisconnected);
    CHECK(manager.failed_unit() == &b);
    CHECK(log.equals(std::array<std::int32_t, 3U>{{12, 22, 13}}));  // a started, b failed, a stopped.
    CHECK(a.state() == common::UnitState::kInitialized);

    b.start_result = common::Status::kOk;
    CHECK(manager.start() == common::Status::kOk);
    CHECK(manager.start() == common::Status::kAlreadyRunning);
    CHECK(manager.failed_unit() == &a);
    manager.shutdown();
}

TEST_CASE(unit_manager_find_duplicates_clear_and_destructor) {
    HookLog log{};
    LoggingUnit a{"alpha", 1, log};
    LoggingUnit same_name{"alpha", 2, log};
    LoggingUnit b{"beta", 3, log};
    {
        common::UnitManager<3U> manager{};
        CHECK(manager.capacity() == 3U);
        REQUIRE(manager.add(a) == common::Status::kOk);
        CHECK(manager.add(a) == common::Status::kAlreadyExists);
        CHECK(manager.add(same_name) == common::Status::kAlreadyExists);
        REQUIRE(manager.add(b) == common::Status::kOk);
        CHECK(manager.size() == 2U);
        CHECK(manager.find("beta") == &b);
        CHECK(manager.find("bet") == nullptr);
        CHECK(manager.find("betas") == nullptr);
        CHECK(manager.find(std::string_view{"beta\0\0", 6U}) == nullptr);  // A NUL never matches.
        const common::UnitManager<3U>& view{manager};
        CHECK(view.find("alpha") == &a);

        manager.clear();
        CHECK(manager.size() == 0U);
        REQUIRE(manager.add(a) == common::Status::kOk);
        REQUIRE(manager.initialize() == common::Status::kOk);
        REQUIRE(manager.start() == common::Status::kOk);
        log.clear();
    }
    // The destructor shut the running unit down.
    CHECK(a.state() == common::UnitState::kCreated);
    CHECK(log.equals(std::array<std::int32_t, 2U>{{13, 14}}));
}
