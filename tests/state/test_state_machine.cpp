#include <array>
#include <cstddef>
#include <cstdint>

#include "common/state/state_machine.hpp"
#include "test_framework.hpp"

namespace {

// ---- Operating modes ----------------------------------------------------------------------

enum class Mode : std::uint8_t { kInit, kRun, kDegraded, kSafe };
enum class ModeEvent : std::uint8_t { kReady, kSensorLost, kSensorBack, kFault, kPing, kUnused };

class Controller;
using ModeMachine = common::StateMachine<Mode, ModeEvent, Controller>;

/// The context: counts what the machine made it do, and remembers what it saw of the machine.
class Controller final {
public:
    std::int32_t entered_run{0};
    std::int32_t left_run{0};
    std::int32_t entered_safe{0};
    std::int32_t torque_limits{0};
    std::int32_t pings{0};
    bool sensor_ok{true};
    const ModeMachine* machine{nullptr};
    Mode state_in_action{Mode::kInit};
    Mode state_in_exit{Mode::kInit};
    Mode state_in_entry{Mode::kInit};
    std::array<ModeEvent, 8U> observed{};
    std::size_t observed_count{0U};
};

void enter_run(Controller& controller) noexcept {
    ++controller.entered_run;
    controller.state_in_entry = controller.machine->state();
}

void leave_run(Controller& controller) noexcept {
    ++controller.left_run;
    controller.state_in_exit = controller.machine->state();
}

void enter_safe(Controller& controller) noexcept {
    ++controller.entered_safe;
}

void limit_torque(Controller& controller, ModeEvent /*event*/, const common::NoEventData& /*data*/) noexcept {
    ++controller.torque_limits;
    controller.state_in_action = controller.machine->state();
}

void count_ping(Controller& controller, ModeEvent /*event*/, const common::NoEventData& /*data*/) noexcept {
    ++controller.pings;
}

bool sensor_ok(const Controller& controller, ModeEvent /*event*/, const common::NoEventData& /*data*/) noexcept {
    return controller.sensor_ok;
}

bool sensor_not_ok(const Controller& controller, ModeEvent /*event*/, const common::NoEventData& /*data*/) noexcept {
    return !controller.sensor_ok;
}

void observe(Controller& controller, Mode /*from*/, const ModeEvent event, Mode /*to*/) noexcept {
    if (controller.observed_count < controller.observed.size()) {
        controller.observed[controller.observed_count] = event;
        ++controller.observed_count;
    }
}

// kReady goes to kRun when the sensor is fine and to kDegraded when it is not (first match wins,
// guards choose). kPing is internal in kRun and external (self) in kDegraded.
constexpr std::array<ModeMachine::Transition, 8U> kModeTransitions{{
    {Mode::kInit, ModeEvent::kReady, Mode::kRun, &sensor_ok},
    {Mode::kInit, ModeEvent::kReady, Mode::kDegraded, &sensor_not_ok},
    {Mode::kRun, ModeEvent::kSensorLost, Mode::kDegraded, nullptr, &limit_torque},
    {Mode::kDegraded, ModeEvent::kSensorBack, Mode::kRun},
    {Mode::kRun, ModeEvent::kFault, Mode::kSafe},
    {Mode::kDegraded, ModeEvent::kFault, Mode::kSafe},
    {Mode::kRun, ModeEvent::kPing, Mode::kRun, nullptr, &count_ping, common::TransitionKind::kInternal},
    {Mode::kDegraded, ModeEvent::kPing, Mode::kDegraded, nullptr, &count_ping},
}};

constexpr std::array<ModeMachine::StateActions, 3U> kModeActions{{
    {Mode::kRun, &enter_run, &leave_run},
    {Mode::kSafe, &enter_safe},
    {Mode::kDegraded, [](Controller& controller) noexcept { controller.state_in_entry = controller.machine->state(); }},
}};

// ---- An order, with event data -------------------------------------------------------------

enum class OrderState : std::uint8_t { kNew, kPending, kPartiallyFilled, kFilled, kCancelled };
enum class OrderEvent : std::uint8_t { kSend, kAccepted, kFill, kCancel };

struct Fill final {
    std::int64_t quantity{0};
    std::int64_t price{0};
};

struct Order final {
    std::int64_t quantity{0};
    std::int64_t filled{0};
    std::int64_t notional{0};
};

using OrderMachine = common::StateMachine<OrderState, OrderEvent, Order, Fill>;

bool completes(const Order& order, OrderEvent /*event*/, const Fill& fill) noexcept {
    return (order.filled + fill.quantity) >= order.quantity;
}

void apply_fill(Order& order, OrderEvent /*event*/, const Fill& fill) noexcept {
    order.filled += fill.quantity;
    order.notional += fill.quantity * fill.price;
}

constexpr std::array<OrderMachine::Transition, 7U> kOrderTransitions{{
    {OrderState::kNew, OrderEvent::kSend, OrderState::kPending},
    {OrderState::kPending, OrderEvent::kFill, OrderState::kFilled, &completes, &apply_fill},
    {OrderState::kPending, OrderEvent::kFill, OrderState::kPartiallyFilled, nullptr, &apply_fill},
    {OrderState::kPartiallyFilled, OrderEvent::kFill, OrderState::kFilled, &completes, &apply_fill},
    {OrderState::kPartiallyFilled, OrderEvent::kFill, OrderState::kPartiallyFilled, nullptr, &apply_fill,
     common::TransitionKind::kInternal},
    {OrderState::kPending, OrderEvent::kCancel, OrderState::kCancelled},
    {OrderState::kPartiallyFilled, OrderEvent::kCancel, OrderState::kCancelled},
}};

constexpr std::array<OrderMachine::StateActions, 0U> kNoOrderActions{};

}  // namespace

TEST_CASE(state_machine_modes_and_actions) {
    const ModeMachine::Definition definition{kModeTransitions, kModeActions, &observe};
    Controller controller{};
    ModeMachine modes{definition, Mode::kInit, controller};
    controller.machine = &modes;
    CHECK(!modes.started());
    CHECK(modes.state() == Mode::kInit);

    modes.start();
    CHECK(modes.started());
    CHECK(modes.state() == Mode::kInit);  // No entry action for kInit.

    CHECK(modes.dispatch(ModeEvent::kReady));
    CHECK(modes.state() == Mode::kRun);
    CHECK(controller.entered_run == 1);
    CHECK(controller.state_in_entry == Mode::kRun);  // The target, from the entry action on.

    CHECK(modes.dispatch(ModeEvent::kSensorLost));
    CHECK(modes.state() == Mode::kDegraded);
    CHECK(controller.left_run == 1);
    CHECK(controller.state_in_exit == Mode::kRun);    // The source during the exit action ...
    CHECK(controller.state_in_action == Mode::kRun);  // ... and the transition action.
    CHECK(controller.torque_limits == 1);
    CHECK(controller.state_in_entry == Mode::kDegraded);

    CHECK(modes.dispatch(ModeEvent::kSensorBack));
    CHECK(modes.dispatch(ModeEvent::kFault));
    CHECK(modes.state() == Mode::kSafe);
    CHECK(controller.entered_safe == 1);
    CHECK(controller.left_run == 2);

    // The observer saw every fired transition, in order.
    REQUIRE(controller.observed_count == 4U);
    CHECK(controller.observed[0U] == ModeEvent::kReady);
    CHECK(controller.observed[3U] == ModeEvent::kFault);
}

TEST_CASE(state_machine_ignored_events_and_guards) {
    const ModeMachine::Definition definition{kModeTransitions, kModeActions, &observe};
    Controller controller{};
    controller.sensor_ok = false;
    ModeMachine modes{definition, Mode::kInit, controller};
    controller.machine = &modes;
    CHECK(!modes.can_dispatch(ModeEvent::kReady));  // Not started.
    modes.start();

    CHECK(!modes.dispatch(ModeEvent::kSensorBack));  // No row for (kInit, kSensorBack).
    CHECK(!modes.dispatch(ModeEvent::kUnused));
    CHECK(modes.state() == Mode::kInit);
    CHECK(controller.observed_count == 0U);  // Ignored events are not observed.

    CHECK(modes.can_dispatch(ModeEvent::kReady));
    CHECK(!modes.can_dispatch(ModeEvent::kFault));
    CHECK(modes.dispatch(ModeEvent::kReady));  // The guard picks the second row.
    CHECK(modes.state() == Mode::kDegraded);
}

TEST_CASE(state_machine_internal_and_external_self_transitions) {
    const ModeMachine::Definition definition{kModeTransitions, kModeActions};
    Controller controller{};
    ModeMachine modes{definition, Mode::kInit, controller};
    controller.machine = &modes;
    modes.start();
    CHECK(modes.dispatch(ModeEvent::kReady));
    CHECK(controller.entered_run == 1);

    // Internal in kRun: action only.
    CHECK(modes.dispatch(ModeEvent::kPing));
    CHECK(controller.pings == 1);
    CHECK(controller.entered_run == 1);
    CHECK(controller.left_run == 0);
    CHECK(modes.state() == Mode::kRun);

    // External self-transition in kDegraded: exit and entry run again.
    CHECK(modes.dispatch(ModeEvent::kSensorLost));
    controller.state_in_entry = Mode::kInit;
    CHECK(modes.dispatch(ModeEvent::kPing));
    CHECK(controller.pings == 2);
    CHECK(controller.state_in_entry == Mode::kDegraded);  // Its entry action ran.
}

TEST_CASE(state_machine_reset_and_restart) {
    const ModeMachine::Definition definition{kModeTransitions, kModeActions};
    Controller controller{};
    ModeMachine modes{definition, Mode::kRun, controller};
    controller.machine = &modes;
    modes.start();
    CHECK(controller.entered_run == 1);  // start() runs the entry action of the initial state.
    CHECK(modes.dispatch(ModeEvent::kFault));
    CHECK(modes.state() == Mode::kSafe);

    modes.reset();
    CHECK(!modes.started());
    CHECK(modes.state() == Mode::kRun);
    CHECK(controller.left_run == 1);  // reset() runs no exit action (only kFault's exit ran).
    modes.start();
    CHECK(controller.entered_run == 2);
}

TEST_CASE(state_machine_event_data_selects_transitions) {
    const OrderMachine::Definition definition{kOrderTransitions, kNoOrderActions};
    Order order{};
    order.quantity = 100;
    OrderMachine machine{definition, OrderState::kNew, order};
    machine.start();
    CHECK(machine.dispatch(OrderEvent::kSend));
    CHECK(machine.state() == OrderState::kPending);

    CHECK(machine.dispatch(OrderEvent::kFill, Fill{30, 101}));
    CHECK(machine.state() == OrderState::kPartiallyFilled);
    CHECK(machine.dispatch(OrderEvent::kFill, Fill{30, 102}));  // Internal: stays partially filled.
    CHECK(machine.state() == OrderState::kPartiallyFilled);
    CHECK(!machine.can_dispatch(OrderEvent::kSend));
    CHECK(machine.can_dispatch(OrderEvent::kFill, Fill{40, 0}));
    CHECK(machine.dispatch(OrderEvent::kFill, Fill{40, 100}));
    CHECK(machine.state() == OrderState::kFilled);
    CHECK(order.filled == 100);
    CHECK(order.notional == (30 * 101) + (30 * 102) + (40 * 100));
    CHECK(!machine.dispatch(OrderEvent::kCancel));  // Filled orders cannot be cancelled.
}

TEST_CASE(state_machine_many_machines_share_a_definition) {
    static const OrderMachine::Definition definition{kOrderTransitions, kNoOrderActions};
    static std::array<Order, 1000U> orders{};
    // One machine per order, each constructed in O(1).
    std::int32_t filled{0};
    for (std::size_t i{0U}; i < orders.size(); ++i) {
        orders[i].quantity = static_cast<std::int64_t>(1U + (i % 3U));
        OrderMachine machine{definition, OrderState::kNew, orders[i]};
        machine.start();
        static_cast<void>(machine.dispatch(OrderEvent::kSend));
        static_cast<void>(machine.dispatch(OrderEvent::kFill, Fill{2, 10}));
        if (machine.state() == OrderState::kFilled) {
            ++filled;
        }
    }
    CHECK(filled == 667);  // Quantities 1 and 2 are filled by 2; quantity 3 is not.
}
