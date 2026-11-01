#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/app/unit.hpp"
#include "common/core/status.hpp"
#include "test_framework.hpp"

namespace {

/// Counts its hook calls; each hook can be made to fail.
class CountingUnit final : public common::Unit {
public:
    explicit CountingUnit(const std::string_view name) noexcept : Unit{name} {}

    std::int32_t initialized{0};
    std::int32_t started{0};
    std::int32_t stopped{0};
    std::int32_t shut_down{0};
    common::Status initialize_result{common::Status::kOk};
    common::Status start_result{common::Status::kOk};

private:
    common::Status on_initialize() noexcept override {
        ++initialized;
        return initialize_result;
    }
    common::Status on_start() noexcept override {
        ++started;
        return start_result;
    }
    void on_stop() noexcept override {
        ++stopped;
    }
    void on_shutdown() noexcept override {
        ++shut_down;
    }
};

/// Uses every default hook.
class PlainUnit final : public common::Unit {
public:
    PlainUnit() noexcept : Unit{"plain"} {}
};

}  // namespace

TEST_CASE(unit_lifecycle) {
    CountingUnit unit{"engine"};
    CHECK(std::string_view{unit.name()} == "engine");
    CHECK(unit.state() == common::UnitState::kCreated);
    CHECK(std::string_view{common::to_string(unit.state())} == "created");

    CHECK(unit.start() == common::Status::kInvalidArgument);  // Not initialised.
    CHECK(unit.started == 0);
    CHECK(unit.initialize() == common::Status::kOk);
    CHECK(unit.state() == common::UnitState::kInitialized);
    CHECK(unit.initialize() == common::Status::kInvalidArgument);  // Called in the wrong state.
    CHECK(unit.initialized == 1);

    CHECK(unit.start() == common::Status::kOk);
    CHECK(unit.state() == common::UnitState::kRunning);
    CHECK(unit.start() == common::Status::kAlreadyRunning);

    unit.stop();
    CHECK(unit.state() == common::UnitState::kInitialized);
    unit.stop();  // Not running: nothing happens.
    CHECK(unit.stopped == 1);
    CHECK(unit.start() == common::Status::kOk);  // Restarted.

    unit.shutdown();  // Stops first.
    CHECK(unit.stopped == 2);
    CHECK(unit.shut_down == 1);
    CHECK(unit.state() == common::UnitState::kCreated);
    unit.shutdown();  // Nothing left to do.
    CHECK(unit.shut_down == 1);

    CHECK(unit.initialize() == common::Status::kOk);  // Initialised again after a shutdown.
    unit.shutdown();
}

TEST_CASE(unit_failing_hooks_keep_the_state) {
    CountingUnit unit{"gateway"};
    unit.initialize_result = common::Status::kNotFound;
    CHECK(unit.initialize() == common::Status::kNotFound);
    CHECK(unit.state() == common::UnitState::kCreated);

    unit.initialize_result = common::Status::kOk;
    CHECK(unit.initialize() == common::Status::kOk);
    unit.start_result = common::Status::kDisconnected;
    CHECK(unit.start() == common::Status::kDisconnected);
    CHECK(unit.state() == common::UnitState::kInitialized);
    unit.shutdown();
    CHECK(unit.stopped == 0);  // It never ran.
    CHECK(unit.shut_down == 1);
}

TEST_CASE(unit_default_hooks) {
    PlainUnit unit{};
    CHECK(unit.initialize() == common::Status::kOk);
    CHECK(unit.start() == common::Status::kOk);
    unit.shutdown();
    CHECK(unit.state() == common::UnitState::kCreated);
    CHECK(std::string_view{common::to_string(common::UnitState::kRunning)} == "running");
}

TEST_CASE(unit_dependencies) {
    CountingUnit unit{"control"};
    std::array<CountingUnit*, common::kMaxUnitDependencies + 1U> others{};
    static CountingUnit d0{"d0"};
    static CountingUnit d1{"d1"};
    static CountingUnit d2{"d2"};
    static CountingUnit d3{"d3"};
    static CountingUnit d4{"d4"};
    static CountingUnit d5{"d5"};
    static CountingUnit d6{"d6"};
    static CountingUnit d7{"d7"};
    static CountingUnit d8{"d8"};
    others = {{&d0, &d1, &d2, &d3, &d4, &d5, &d6, &d7, &d8}};

    CHECK(unit.depends_on(unit) == common::Status::kInvalidArgument);
    CHECK(unit.depends_on(d0) == common::Status::kOk);
    CHECK(unit.depends_on(d0) == common::Status::kAlreadyExists);
    for (std::size_t i{1U}; i < common::kMaxUnitDependencies; ++i) {
        CHECK(unit.depends_on(*others[i]) == common::Status::kOk);
    }
    CHECK(unit.dependency_count() == common::kMaxUnitDependencies);
    CHECK(unit.depends_on(*others[common::kMaxUnitDependencies]) == common::Status::kResourceLimit);
    CHECK(&unit.dependency(0U) == &d0);
    CHECK(&unit.dependency(7U) == &d7);
}

TEST_CASE(unit_names) {
    CHECK(common::Unit::is_valid_name("can0"));
    CHECK(!common::Unit::is_valid_name(""));
    CHECK(common::Unit::is_valid_name("0123456789012345678901234567890"));     // 31 characters
    CHECK(!common::Unit::is_valid_name("01234567890123456789012345678901"));   // 32
    const std::array<char, 3U> with_nul{{'a', '\0', 'b'}};
    CHECK(!common::Unit::is_valid_name(std::string_view{with_nul.data(), with_nul.size()}));

    // A name read into a reused buffer is copied.
    std::array<char, 8U> buffer{{'c', 'a', 'n', '1', '\0', '\0', '\0', '\0'}};
    const CountingUnit unit{std::string_view{buffer.data(), 4U}};
    buffer.fill('x');
    CHECK(std::string_view{unit.name()} == "can1");
}
