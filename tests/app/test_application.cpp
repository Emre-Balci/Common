// The patterns of common working together, as applications use them: units owning tasks, an active
// object and a state machine; supervision with a Watchdog; order state machines driven by an active
// object with a timer wheel; units created from configuration; a final job at shutdown. Real threads,
// but no verdict depends on timing.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>

#include "common/app/unit.hpp"
#include "common/app/unit_manager.hpp"
#include "common/containers/object_pool.hpp"
#include "common/core/status.hpp"
#include "common/diagnostics/watchdog.hpp"
#include "common/event/signal.hpp"
#include "common/object/object_type.hpp"
#include "common/object/object_type_registry.hpp"
#include "common/state/state_machine.hpp"
#include "common/system/thread.hpp"
#include "common/task/active_object.hpp"
#include "common/task/task.hpp"
#include "common/task/task_executor.hpp"
#include "common/task/task_scheduler.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "common/time/timer_wheel.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

common::ThreadConfig thread_config(const char* const name) noexcept {
    common::ThreadConfig config{};
    config.name = name;
    config.policy = common::SchedPolicy::kOther;
    config.priority = 0;
    return config;
}

template <typename Done>
bool eventually(const Done& done) noexcept {
    const common::Deadline deadline{common::Deadline::after(10s)};
    for (std::int32_t i{0}; (i < 1'000'000) && (!done()) && (!deadline.expired()); ++i) {
        std::this_thread::sleep_for(200us);
    }
    return done();
}

// ---- An ECU-like application -------------------------------------------------------------

enum class Mode : std::uint8_t { kInit, kRun, kSafe };
enum class ModeEvent : std::uint8_t { kReady, kFault };

struct ModeContext final {
    std::atomic<Mode> current{Mode::kInit};
};

using ModeMachine = common::StateMachine<Mode, ModeEvent, ModeContext>;

void publish_mode(ModeContext& context, Mode /*from*/, ModeEvent /*event*/, const Mode to) noexcept {
    context.current.store(to, std::memory_order_release);
}

constexpr std::array<ModeMachine::Transition, 2U> kModeTransitions{{
    {Mode::kInit, ModeEvent::kReady, Mode::kRun},
    {Mode::kRun, ModeEvent::kFault, Mode::kSafe},
}};
constexpr std::array<ModeMachine::StateActions, 0U> kNoModeActions{};

/// Owns the scheduler and its thread; the units that have tasks depend on it.
class RuntimeUnit final : public common::Unit {
public:
    RuntimeUnit() noexcept : Unit{"runtime"} {}

    [[nodiscard]] common::TaskScheduler<4U>& scheduler() noexcept {
        return scheduler_;
    }

private:
    common::Status on_start() noexcept override {
        return scheduler_.start(thread_config("runtime"), 1ms);
    }
    void on_stop() noexcept override {
        static_cast<void>(scheduler_.stop());
    }

    common::TaskScheduler<4U> scheduler_{};
};

/// Owns the mode state machine, which runs on its active object's thread.
class ModeUnit final : public common::Unit {
public:
    ModeUnit() noexcept : Unit{"modes"} {}

    /// Any thread: asks for a mode change.
    common::Status request(const ModeEvent event) noexcept {
        return active_.post([this, event]() noexcept { static_cast<void>(machine_.dispatch(event)); });
    }

    [[nodiscard]] Mode mode() const noexcept {
        return context_.current.load(std::memory_order_acquire);
    }

private:
    common::Status on_start() noexcept override {
        // The machine belongs to the object's thread: its first command enters the initial state.
        common::Status status{active_.post([this]() noexcept { machine_.start(); })};
        if (common::is_ok(status)) {
            status = active_.start(thread_config("modes"));
        }
        return status;
    }
    void on_stop() noexcept override {
        static_cast<void>(active_.stop(common::StopMode::kDrain));
        machine_.reset();  // The thread has stopped: the machine is the control thread's again.
    }

    ModeContext context_{};
    const ModeMachine::Definition definition_{kModeTransitions, kNoModeActions, &publish_mode};
    ModeMachine machine_{definition_, Mode::kInit, context_};
    common::ActiveObject<16U> active_{};
};

/// A periodic control task that may only run while its unit operates.
class ControlTask final : public common::Task {
public:
    explicit ControlTask(ModeUnit& modes) noexcept : Task{"control"}, modes_{&modes} {}

    std::atomic<bool> operating{false};
    std::atomic<bool> ran_outside{false};
    std::atomic<std::int32_t> runs{0};
    common::Signal<void(std::int32_t), 2U> speed{};  // Notified on the scheduler's thread.

private:
    common::Status run(const common::TaskActivation& /*activation*/) noexcept override {
        if (!operating.load(std::memory_order_acquire)) {
            ran_outside.store(true, std::memory_order_relaxed);
        }
        const std::int32_t run_count{runs.fetch_add(1, std::memory_order_relaxed) + 1};
        if (run_count == 5) {
            static_cast<void>(modes_->request(ModeEvent::kReady));  // Ready after five cycles.
        }
        speed.notify(run_count);
        return common::Status::kOk;
    }

    ModeUnit* modes_;
};

/// Owns the control task and maps it onto its lifecycle ("Owning parts" in unit.hpp).
class ControlUnit final : public common::Unit {
public:
    ControlUnit(RuntimeUnit& runtime, ModeUnit& modes) noexcept : Unit{"control"}, runtime_{&runtime}, task_{modes} {}

    [[nodiscard]] ControlTask& task() noexcept {
        return task_;
    }
    std::atomic<std::int32_t> last_speed{0};

private:
    common::Status on_initialize() noexcept override {
        common::TaskSchedule schedule{common::TaskSchedule::periodic(2ms)};
        schedule.suspended = true;
        common::Status status{runtime_->scheduler().add(task_, schedule)};
        if (common::is_ok(status)) {
            const common::Result<common::Connection> connection{task_.speed.connect(
                [this](const std::int32_t speed) noexcept { last_speed.store(speed, std::memory_order_relaxed); })};
            status = connection.status();
            if (connection.has_value()) {
                connection_ = common::ScopedConnection{task_.speed, connection.value()};
            } else {
                static_cast<void>(runtime_->scheduler().remove(task_));  // Undo what this hook did.
            }
        }
        return status;
    }
    common::Status on_start() noexcept override {
        task_.operating.store(true, std::memory_order_release);
        task_.resume();
        return common::Status::kOk;
    }
    void on_stop() noexcept override {
        task_.suspend();
        static_cast<void>(common::wait_until_state(task_, common::TaskState::kSuspended, common::Deadline::after(10s)));
        task_.operating.store(false, std::memory_order_release);
    }
    void on_shutdown() noexcept override {
        static_cast<void>(runtime_->scheduler().remove(task_));
        connection_.reset();
    }

    RuntimeUnit* runtime_;
    ControlTask task_;
    common::ScopedConnection connection_{};
};

// ---- A trading-like gateway ---------------------------------------------------------------

enum class OrderState : std::uint8_t { kNew, kPending, kAccepted, kPartiallyFilled, kFilled, kRejected };
enum class OrderEvent : std::uint8_t { kSend, kAck, kFill, kTimeout };

struct Fill final {
    std::int64_t quantity{0};
};

struct Order final {
    std::int64_t quantity{100};
    std::int64_t filled{0};
    common::TimerId timeout{};
};

using OrderMachine = common::StateMachine<OrderState, OrderEvent, Order, Fill>;

bool completes(const Order& order, OrderEvent /*event*/, const Fill& fill) noexcept {
    return (order.filled + fill.quantity) >= order.quantity;
}

void add_fill(Order& order, OrderEvent /*event*/, const Fill& fill) noexcept {
    order.filled += fill.quantity;
}

constexpr std::array<OrderMachine::Transition, 7U> kOrderTransitions{{
    {OrderState::kNew, OrderEvent::kSend, OrderState::kPending},
    {OrderState::kPending, OrderEvent::kAck, OrderState::kAccepted},
    {OrderState::kPending, OrderEvent::kTimeout, OrderState::kRejected},
    {OrderState::kAccepted, OrderEvent::kFill, OrderState::kFilled, &completes, &add_fill},
    {OrderState::kAccepted, OrderEvent::kFill, OrderState::kPartiallyFilled, nullptr, &add_fill},
    {OrderState::kPartiallyFilled, OrderEvent::kFill, OrderState::kFilled, &completes, &add_fill},
    {OrderState::kPartiallyFilled, OrderEvent::kFill, OrderState::kPartiallyFilled, nullptr, &add_fill,
     common::TransitionKind::kInternal},
}};
constexpr std::array<OrderMachine::StateActions, 0U> kNoOrderActions{};
const OrderMachine::Definition kOrderDefinition{kOrderTransitions, kNoOrderActions};

/// Talks to an "exchange" on its own thread: orders, acknowledgement timeouts (a TimerWheel advanced in
/// the poll hook) and the order state machines are touched by that thread only.
class OrderGateway final {
public:
    static constexpr std::size_t kOrders{3U};

    OrderGateway() noexcept {
        for (Slot& slot : slots_) {
            slot.gateway = this;
        }
    }

    common::Status start() noexcept {
        return active_.start(thread_config("gateway"), 1ms, &OrderGateway::poll, this);
    }
    common::Status stop() noexcept {
        return active_.stop(common::StopMode::kDrain);
    }

    common::Status send(const std::size_t index) noexcept {
        return active_.post([this, index]() noexcept {
            Slot& slot{slots_[index]};
            slot.machine.start();
            static_cast<void>(slot.machine.dispatch(OrderEvent::kSend));
            const common::Result<common::TimerId> timer{
                timeouts_.schedule(common::MonotonicClock::now() + 20ms, &OrderGateway::on_timeout, &slot)};
            slot.order.timeout = timer.value_or(common::TimerId{});
        });
    }

    common::Status ack(const std::size_t index) noexcept {
        return active_.post([this, index]() noexcept {
            Slot& slot{slots_[index]};
            static_cast<void>(timeouts_.cancel(slot.order.timeout));
            static_cast<void>(slot.machine.dispatch(OrderEvent::kAck));
        });
    }

    common::Status fill(const std::size_t index, const std::int64_t quantity) noexcept {
        return active_.post([this, index, quantity]() noexcept {
            static_cast<void>(slots_[index].machine.dispatch(OrderEvent::kFill, Fill{quantity}));
        });
    }

    /// After stop(): the thread has ended, so the states can be read.
    [[nodiscard]] OrderState state(const std::size_t index) const noexcept {
        return slots_[index].machine.state();
    }

    std::atomic<std::int32_t> timeouts{0};

private:
    struct Slot final {
        Slot() noexcept : machine{kOrderDefinition, OrderState::kNew, order} {}
        Order order{};
        OrderMachine machine;
        OrderGateway* gateway{nullptr};
    };

    static void poll(const common::TimePoint now, void* const self) noexcept {
        static_cast<void>(static_cast<OrderGateway*>(self)->timeouts_.advance(now, 8U));
    }

    static void on_timeout(common::TimerId /*id*/, void* const context) noexcept {
        Slot* const slot{static_cast<Slot*>(context)};
        static_cast<void>(slot->machine.dispatch(OrderEvent::kTimeout));
        slot->gateway->timeouts.fetch_add(1, std::memory_order_relaxed);
    }

    std::array<Slot, kOrders> slots_{};
    common::TimerWheel<8U, 64U> timeouts_{1ms, common::MonotonicClock::now()};
    common::ActiveObject<32U> active_{};
};

// ---- Units created from configuration ------------------------------------------------------

struct GatewayArguments final {
    std::string_view name{"gateway"};
    std::int32_t bus{0};
};

class GatewayUnit final : public common::Unit, public common::Object {
public:
    static constexpr common::ObjectTypeOf<GatewayUnit, common::Object> kType{"GatewayUnit"};

    GatewayUnit(const common::ObjectType& /*type*/, const void* const arguments) noexcept
        : Unit{name_of(arguments)}, bus_{bus_of(arguments)} {}

    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return kType;
    }
    [[nodiscard]] std::int32_t bus() const noexcept {
        return bus_;
    }
    std::int32_t started{0};

private:
    static std::string_view name_of(const void* const arguments) noexcept {
        return (arguments != nullptr) ? static_cast<const GatewayArguments*>(arguments)->name : std::string_view{"gateway"};
    }
    static std::int32_t bus_of(const void* const arguments) noexcept {
        return (arguments != nullptr) ? static_cast<const GatewayArguments*>(arguments)->bus : 0;
    }
    common::Status on_start() noexcept override {
        ++started;
        return common::Status::kOk;
    }

    std::int32_t bus_;
};

/// The job a persistence unit submits at shutdown.
class SnapshotJob final : public common::Task {
public:
    SnapshotJob() noexcept : Task{"snapshot"} {}
    std::atomic<std::int32_t> written{0};

private:
    common::Status run(const common::TaskActivation& /*activation*/) noexcept override {
        std::this_thread::sleep_for(5ms);  // "Writes" the snapshot.
        written.fetch_add(1, std::memory_order_relaxed);
        return common::Status::kOk;
    }
};

}  // namespace

TEST_CASE(application_units_tasks_modes_and_supervision) {
    static RuntimeUnit runtime{};
    static ModeUnit modes{};
    static ControlUnit control{runtime, modes};
    static common::UnitManager<4U> units{};
    REQUIRE(control.depends_on(runtime) == common::Status::kOk);
    REQUIRE(control.depends_on(modes) == common::Status::kOk);
    REQUIRE(units.add(control) == common::Status::kOk);  // Added first, started last.
    REQUIRE(units.add(runtime) == common::Status::kOk);
    REQUIRE(units.add(modes) == common::Status::kOk);

    // Supervision: the scheduler's heartbeat is parked until it runs, so the watchdog stays quiet.
    common::Watchdog<1U> watchdog{};
    const common::TimePoint t0{common::MonotonicClock::now()};
    REQUIRE(watchdog.add(runtime.scheduler().heartbeat(), 500ms, "scheduler", t0).has_value());
    CHECK(units.initialize() == common::Status::kOk);
    CHECK(watchdog.check(t0 + 10s) == 0U);

    CHECK(units.start() == common::Status::kOk);
    CHECK(&units[2U] == &control);
    CHECK(eventually([]() noexcept { return control.task().runs.load(std::memory_order_relaxed) >= 10; }));
    CHECK(eventually([]() noexcept { return modes.mode() == Mode::kRun; }));
    CHECK(control.last_speed.load(std::memory_order_relaxed) > 0);
    CHECK(watchdog.check() == 0U);  // Alive while running.

    units.stop();
    CHECK(control.task().state() == common::TaskState::kSuspended);
    const std::int32_t runs_at_stop{control.task().runs.load(std::memory_order_relaxed)};
    units.shutdown();
    CHECK(control.task().state() == common::TaskState::kIdle);
    CHECK(control.task().runs.load(std::memory_order_relaxed) == runs_at_stop);  // Nothing ran after stop().
    CHECK(!control.task().ran_outside.load(std::memory_order_relaxed));  // Never outside [on_start, on_stop).
    CHECK(watchdog.check(common::MonotonicClock::now() + 10s) == 0U);    // Parked again after the stop.
    CHECK(runtime.state() == common::UnitState::kCreated);
}

TEST_CASE(application_order_gateway) {
    static OrderGateway gateway{};
    REQUIRE(gateway.start() == common::Status::kOk);
    for (std::size_t i{0U}; i < OrderGateway::kOrders; ++i) {
        REQUIRE(gateway.send(i) == common::Status::kOk);
    }
    REQUIRE(gateway.ack(0U) == common::Status::kOk);
    REQUIRE(gateway.fill(0U, 100) == common::Status::kOk);  // Filled at once.
    REQUIRE(gateway.ack(1U) == common::Status::kOk);
    REQUIRE(gateway.fill(1U, 40) == common::Status::kOk);  // Partially filled.
    // Order 2 is never acknowledged: its timer fires in the poll hook and rejects it.
    CHECK(eventually([]() noexcept { return gateway.timeouts.load(std::memory_order_relaxed) == 1; }));
    CHECK(gateway.stop() == common::Status::kOk);
    CHECK(gateway.state(0U) == OrderState::kFilled);
    CHECK(gateway.state(1U) == OrderState::kPartiallyFilled);
    CHECK(gateway.state(2U) == OrderState::kRejected);
}

TEST_CASE(application_units_from_configuration) {
    static common::ObjectPool<GatewayUnit, 2U> gateways{};
    common::ObjectTypeRegistry<4U> types{};
    REQUIRE(types.add(GatewayUnit::kType, common::pool_factory(gateways)) == common::Status::kOk);

    // A configuration: (type, name, bus) per line.
    struct Line final {
        std::string_view type;
        GatewayArguments arguments;
    };
    const std::array<Line, 3U> configuration{{
        {"GatewayUnit", GatewayArguments{"can0", 0}},
        {"GatewayUnit", GatewayArguments{"can1", 1}},
        {"FlexRayUnit", GatewayArguments{"fr0", 2}},
    }};
    common::UnitManager<4U> units{};
    std::array<GatewayUnit*, 3U> created{};
    std::int32_t unknown{0};
    for (std::size_t i{0U}; i < configuration.size(); ++i) {
        REQUIRE(common::Unit::is_valid_name(configuration[i].arguments.name));
        const common::Result<GatewayUnit*> unit{types.create_as<GatewayUnit>(configuration[i].type,
                                                                            &configuration[i].arguments)};
        if (unit.has_value()) {
            created[i] = unit.value();
            REQUIRE(units.add(*unit.value()) == common::Status::kOk);
        } else if (unit.status() == common::Status::kNotFound) {
            ++unknown;
        } else {
            CHECK(false);
        }
    }
    CHECK(unknown == 1);
    REQUIRE(units.size() == 2U);
    GatewayUnit* const can1{static_cast<GatewayUnit*>(units.find("can1"))};
    REQUIRE(can1 != nullptr);
    CHECK(can1->bus() == 1);
    CHECK(units.initialize() == common::Status::kOk);
    CHECK(units.start() == common::Status::kOk);
    CHECK(created[0U]->started == 1);
    units.shutdown();
    units.clear();
    CHECK(types.destroy(*created[0U]) == common::Status::kOk);
    CHECK(types.destroy(*created[1U]) == common::Status::kOk);
    CHECK(gateways.in_use() == 0U);
}

TEST_CASE(application_final_job_at_shutdown) {
    static SnapshotJob snapshot{};
    static common::TaskExecutor<1U, 8U> background{};
    REQUIRE(background.start(thread_config("background")) == common::Status::kOk);
    REQUIRE(background.submit(snapshot) == common::Status::kOk);  // Just before the stop.
    CHECK(background.stop(common::StopMode::kDrain) == common::Status::kOk);
    CHECK(snapshot.state() == common::TaskState::kDone);
    CHECK(snapshot.written.load(std::memory_order_relaxed) == 1);
}
