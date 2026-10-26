/// @file state_machine.hpp
/// @brief Table-driven finite state machine with guards, transition actions and entry/exit actions.

#ifndef COMMON_STATE_STATE_MACHINE_HPP
#define COMMON_STATE_STATE_MACHINE_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "common/containers/span.hpp"
#include "common/core/assert.hpp"

namespace common {

/// How a transition treats the states it connects.
enum class TransitionKind : std::uint8_t {
    kExternal,  ///< Exit action of the source, transition action, entry action of the target; also when they are the same state.
    kInternal,  ///< The transition action only: the state does not change. The target must equal the source.
};

/// Event data of a state machine whose events carry none.
struct NoEventData final {};

/// A finite state machine described by tables: for each (source state, event) the transitions that
/// may fire, each with an optional guard and action, plus optional entry and exit actions per state.
/// Typical uses: operating modes (init, run, degraded, safe state), the life of an order (new,
/// pending, partially filled, filled, cancelled), a protocol session.
///
/// - State and Event are enumerations. Data is what events carry (e.g. a fill's quantity and price);
///   guards and actions receive it with the event.
/// - The tables live in a Definition, which validates them once during initialisation and is shared
///   by any number of machines: one per order costs a state, two pointers and a flag, and is
///   constructed in O(1) on the real-time path.
/// - dispatch(event) fires the first transition, in table order, whose source is the current state,
///   whose event matches and whose guard (if any) allows it: for an external transition the exit
///   action of the source, the transition action, the new state, the entry action of the target;
///   for an internal one the action only. Then the Definition's observer, if any, sees (from, event,
///   to): one place for an audit trail or for publishing mode changes. An event without such a
///   transition is ignored (dispatch() returns false; the observer is not called).
/// - Run to completion: start(), dispatch() and reset() must not be called from a guard, an action or
///   the observer of the same machine (no recursion; COMMON_ASSERT). An action that needs a follow-up
///   event posts it, e.g. as an ActiveObject command, and handles that post() can fail. state(),
///   started() and can_dispatch() may be called from them: state() is the source during the guards,
///   the exit action and the transition action, and the target from the entry action on (the source
///   throughout an internal transition).
/// - Guards take a const Context and must not have side effects; can_dispatch() evaluates them
///   without firing anything.
///
/// Write the tables as static constexpr std::arrays of function pointers (captureless noexcept
/// lambdas convert to them) at namespace scope after the Context class: as static constexpr members
/// of the Context class itself they do not compile with Clang 10 (member access into an incomplete
/// type).
///
/// RT-path safe: everything but the Definition's constructor (initialisation phase); dispatch() is
/// O(transitions + states). Not thread-safe: a machine belongs to one thread (the Definition is
/// immutable and can be shared by machines on several threads).
///
/// @code
///   enum class Mode : std::uint8_t { kInit, kRun, kDegraded, kSafe };
///   enum class ModeEvent : std::uint8_t { kReady, kSensorLost, kSensorBack, kFault };
///   class Controller { ... };  // the Context: limits the actuators, logs, ...
///   void limit_torque(Controller& controller, ModeEvent event, const common::NoEventData& data) noexcept;
///   void enter_safe_state(Controller& controller) noexcept;
///   using ModeMachine = common::StateMachine<Mode, ModeEvent, Controller>;
///
///   constexpr std::array<ModeMachine::Transition, 4U> kModeTransitions{{
///       {Mode::kInit, ModeEvent::kReady, Mode::kRun},
///       {Mode::kRun, ModeEvent::kSensorLost, Mode::kDegraded, nullptr, &limit_torque},
///       {Mode::kDegraded, ModeEvent::kSensorBack, Mode::kRun},
///       {Mode::kRun, ModeEvent::kFault, Mode::kSafe},
///   }};
///   constexpr std::array<ModeMachine::StateActions, 1U> kModeActions{{{Mode::kSafe, &enter_safe_state}}};
///   const ModeMachine::Definition kModes{kModeTransitions, kModeActions};   // static storage
///
///   ModeMachine modes{kModes, Mode::kInit, controller};
///   modes.start();
///   static_cast<void>(modes.dispatch(ModeEvent::kReady));   // now in Mode::kRun
/// @endcode
template <typename State, typename Event, typename Context, typename Data = NoEventData>
class StateMachine final {
    static_assert(std::is_enum<State>::value, "State must be an enumeration");
    static_assert(std::is_enum<Event>::value, "Event must be an enumeration");

public:
    /// Decides whether a transition may fire. Must not have side effects.
    using Guard = bool (*)(const Context& context, Event event, const Data& data) noexcept;
    /// Runs when a transition fires.
    using Action = void (*)(Context& context, Event event, const Data& data) noexcept;
    /// Runs when a state is entered or left.
    using StateAction = void (*)(Context& context) noexcept;
    /// Sees every transition that fired, after its entry action.
    using Observer = void (*)(Context& context, State from, Event event, State to) noexcept;

    /// One row of the transition table.
    struct Transition final {
        State source;
        Event event;
        State target;
        Guard guard{nullptr};    ///< nullptr: always allowed.
        Action action{nullptr};  ///< nullptr: none.
        TransitionKind kind{TransitionKind::kExternal};
    };

    /// The entry and exit actions of a state; a state without any needs no row.
    struct StateActions final {
        State state;
        StateAction on_entry{nullptr};
        StateAction on_exit{nullptr};
    };

    /// The immutable part of a machine, shared by any number of machines.
    class Definition final {
    public:
        /// Initialisation phase: validates the tables (COMMON_ASSERT): an internal transition has
        /// target == source, and a state appears at most once in @p state_actions. O(n^2) in the
        /// table sizes. The tables are not copied: keep them alive (static constexpr arrays).
        explicit Definition(const Span<const Transition> transitions, const Span<const StateActions> state_actions,
                            const Observer transition_observer = nullptr) noexcept
            : transitions_{transitions}, state_actions_{state_actions}, observer_{transition_observer} {
            for (const Transition& transition : transitions_) {
                COMMON_ASSERT((transition.kind == TransitionKind::kExternal) || (transition.target == transition.source));
            }
            for (std::size_t i{0U}; i < state_actions_.size(); ++i) {
                for (std::size_t j{i + 1U}; j < state_actions_.size(); ++j) {
                    COMMON_ASSERT(state_actions_[i].state != state_actions_[j].state);
                }
            }
        }

        ~Definition() = default;
        Definition(const Definition&) = delete;
        Definition(Definition&&) = delete;
        Definition& operator=(const Definition&) = delete;
        Definition& operator=(Definition&&) = delete;

        [[nodiscard]] Span<const Transition> transitions() const noexcept {
            return transitions_;
        }

        [[nodiscard]] Observer observer() const noexcept {
            return observer_;
        }

        /// The entry action of @p state, or nullptr.
        [[nodiscard]] StateAction on_entry(const State state) const noexcept {
            const StateActions* const actions{find_actions(state)};
            return (actions != nullptr) ? actions->on_entry : nullptr;
        }

        /// The exit action of @p state, or nullptr.
        [[nodiscard]] StateAction on_exit(const State state) const noexcept {
            const StateActions* const actions{find_actions(state)};
            return (actions != nullptr) ? actions->on_exit : nullptr;
        }

    private:
        [[nodiscard]] const StateActions* find_actions(const State state) const noexcept {
            const StateActions* found{nullptr};
            for (std::size_t i{0U}; (i < state_actions_.size()) && (found == nullptr); ++i) {
                if (state_actions_[i].state == state) {
                    found = &state_actions_[i];
                }
            }
            return found;
        }

        Span<const Transition> transitions_;
        Span<const StateActions> state_actions_;
        Observer observer_;
    };

    /// A machine in @p initial, not started yet. O(1): RT-path safe. @p definition and @p context
    /// must outlive the machine.
    StateMachine(const Definition& definition, const State initial, Context& context) noexcept
        : definition_{&definition}, context_{&context}, initial_{initial}, state_{initial} {}

    ~StateMachine() = default;
    StateMachine(const StateMachine&) = delete;
    StateMachine(StateMachine&&) = delete;
    StateMachine& operator=(const StateMachine&) = delete;
    StateMachine& operator=(StateMachine&&) = delete;

    /// Enters the initial state: state() becomes the initial state, started() true, then its entry
    /// action runs.
    /// @pre !started(); not called from a guard, action or observer of this machine
    void start() noexcept {
        COMMON_ASSERT(!in_transition_);
        COMMON_ASSERT(!started_);
        in_transition_ = true;
        state_ = initial_;
        started_ = true;
        run_state_action(definition_->on_entry(state_));
        in_transition_ = false;
    }

    /// Fires the first transition from state() on @p event whose guard allows it (see the class).
    /// @pre started(); not called from a guard, action or observer of this machine
    /// @return true if a transition fired, false if the event was ignored.
    bool dispatch(const Event event, const Data& data = Data{}) noexcept {
        COMMON_ASSERT(!in_transition_);
        COMMON_ASSERT(started_);
        in_transition_ = true;
        const Transition* const transition{find(event, data)};
        if (transition != nullptr) {
            const State from{state_};
            if (transition->kind == TransitionKind::kExternal) {
                run_state_action(definition_->on_exit(from));
                run_action(*transition, event, data);
                state_ = transition->target;
                run_state_action(definition_->on_entry(state_));
            } else {
                run_action(*transition, event, data);
            }
            const Observer observer{definition_->observer()};
            if (observer != nullptr) {
                observer(*context_, from, event, state_);
            }
        }
        in_transition_ = false;
        return transition != nullptr;
    }

    /// @return true if dispatch(@p event, @p data) would fire a transition now. Evaluates guards only.
    ///         Not to be called from a guard.
    [[nodiscard]] bool can_dispatch(const Event event, const Data& data = Data{}) const noexcept {
        return started_ && (find(event, data) != nullptr);
    }

    /// Back to not started, in the initial state, without running an exit action (e.g. when the unit
    /// that owns the machine is stopped); start() enters the initial state again.
    /// @pre not called from a guard, action or observer of this machine
    void reset() noexcept {
        COMMON_ASSERT(!in_transition_);
        started_ = false;
        state_ = initial_;
    }

    [[nodiscard]] State state() const noexcept {
        return state_;
    }

    [[nodiscard]] bool started() const noexcept {
        return started_;
    }

private:
    [[nodiscard]] const Transition* find(const Event event, const Data& data) const noexcept {
        const Transition* found{nullptr};
        const Span<const Transition> transitions{definition_->transitions()};
        for (std::size_t i{0U}; (i < transitions.size()) && (found == nullptr); ++i) {
            const Transition& transition{transitions[i]};
            if ((transition.source == state_) && (transition.event == event) &&
                ((transition.guard == nullptr) || transition.guard(*context_, event, data))) {
                found = &transition;
            }
        }
        return found;
    }

    void run_action(const Transition& transition, const Event event, const Data& data) noexcept {
        if (transition.action != nullptr) {
            transition.action(*context_, event, data);
        }
    }

    void run_state_action(const StateAction action) noexcept {
        if (action != nullptr) {
            action(*context_);
        }
    }

    const Definition* definition_;
    Context* context_;
    State initial_;
    State state_;
    bool started_{false};
    bool in_transition_{false};
};

}  // namespace common

#endif  // COMMON_STATE_STATE_MACHINE_HPP
