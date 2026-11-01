/// @file unit.hpp
/// @brief Lifecycle unit: a part of an application with initialize/start/stop/shutdown hooks and dependencies.

#ifndef COMMON_APP_UNIT_HPP
#define COMMON_APP_UNIT_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/static_string.hpp"
#include "common/core/status.hpp"

namespace common {

/// Where a unit is in its lifecycle.
enum class UnitState : std::uint8_t {
    kCreated,      ///< Holds no resources: constructed, or shut down.
    kInitialized,  ///< Resources acquired, not running: initialised, or stopped.
    kRunning,      ///< Started.
};

[[nodiscard]] constexpr const char* to_string(const UnitState state) noexcept {
    const char* text{"unknown"};
    switch (state) {
        case UnitState::kCreated:
            text = "created";
            break;
        case UnitState::kInitialized:
            text = "initialized";
            break;
        case UnitState::kRunning:
            text = "running";
            break;
    }
    return text;
}

/// Most dependencies a unit can declare.
inline constexpr std::size_t kMaxUnitDependencies{8U};

/// Longest unit name, in characters.
inline constexpr std::size_t kMaxUnitNameLength{31U};

/// A self-contained part of an application, e.g. a CAN gateway, the engine control, a market-data
/// feed, the persistence of a trading application. A class derives from Unit and overrides the hooks
/// of its lifecycle:
///
///   kCreated --initialize()--> kInitialized --start()--> kRunning
///   kRunning --stop()--> kInitialized --shutdown()--> kCreated
///
/// - on_initialize() acquires the unit's resources during the initialisation phase (it may allocate,
///   block and open files); on_start() begins operating (starts its threads, resumes its tasks);
///   on_stop() ends operating; on_shutdown() releases the resources. A hook that fails undoes what it
///   did and returns the error: the unit stays in its previous state. on_stop() and on_shutdown()
///   cannot fail.
/// - depends_on() declares that a unit needs another: a UnitManager initialises and starts the
///   dependency first, and stops and shuts it down after the unit.
/// - The lifecycle functions (normally called by a UnitManager) and depends_on() belong to one control
///   thread; state() can be read from any thread. A hook must not call a lifecycle function of its own
///   unit (COMMON_ASSERT).
/// - The name is copied (at most kMaxUnitNameLength characters), so units created from configuration
///   can be named from it; check such names first with is_valid_name().
///
/// Owning parts: a unit owns its tasks, active objects and signal connections as members, and maps
/// them onto its lifecycle as follows, so that none of its tasks runs outside [on_start(), on_stop()):
/// - on_initialize(): add its tasks to their TaskScheduler with TaskSchedule::suspended = true;
///   connect its slots (keeping ScopedConnection members); add the heartbeats of its runners (parked
///   until they run) to the supervisor's Watchdog.
/// - on_start(): start its runners (ActiveObject, TaskExecutor, TaskScheduler), then resume() its tasks.
/// - on_stop(): suspend() its tasks and wait_until_state(task, TaskState::kSuspended, deadline), then
///   stop its runners.
/// - on_shutdown(): remove() its tasks from their scheduler and reset() its ScopedConnections (every
///   unit's threads have stopped by then).
/// - A unit that owns a runner other units use (a scheduler, an executor, a supervisor) is a dependency
///   of those units.
/// Prefer this composition to multiple inheritance (deriving from both Unit and Task makes name()
/// ambiguous). A leaf class is final; an intermediate class declares a protected non-virtual
/// destructor and no friends (otherwise -Wnon-virtual-dtor or MSVC's C4265 fails the build).
///
/// @code
///   class ControlUnit final : public common::Unit {
///   public:
///       explicit ControlUnit(RuntimeUnit& runtime) noexcept : Unit{"control"}, runtime_{&runtime} {}
///   private:
///       common::Status on_initialize() noexcept override {
///           common::TaskSchedule schedule{common::TaskSchedule::periodic(std::chrono::milliseconds{10})};
///           schedule.suspended = true;
///           return runtime_->scheduler().add(control_task_, schedule);
///       }
///       common::Status on_start() noexcept override {
///           control_task_.resume();
///           return common::Status::kOk;
///       }
///       void on_stop() noexcept override {
///           control_task_.suspend();
///           static_cast<void>(common::wait_until_state(control_task_, common::TaskState::kSuspended,
///                                                      common::Deadline::after(std::chrono::seconds{1})));
///       }
///       void on_shutdown() noexcept override {
///           static_cast<void>(runtime_->scheduler().remove(control_task_));
///       }
///       RuntimeUnit* runtime_;         // starts the scheduler in its own on_start()
///       ControlTask control_task_{};
///   };
///   // control.depends_on(runtime): the scheduler runs before the control task is resumed.
/// @endcode
class Unit {
public:
    /// @return true if @p name can name a unit: 1 to kMaxUnitNameLength characters, no NUL character.
    [[nodiscard]] static constexpr bool is_valid_name(const std::string_view name) noexcept {
        bool valid{(!name.empty()) && (name.size() <= kMaxUnitNameLength)};
        for (std::size_t i{0U}; valid && (i < name.size()); ++i) {
            valid = (name[i] != '\0');
        }
        return valid;
    }

    /// The unit's own copy of its name.
    [[nodiscard]] const char* name() const noexcept {
        return name_.c_str();
    }

    /// Any thread.
    [[nodiscard]] UnitState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    /// Declares that this unit needs @p dependency (see the class). Initialisation phase.
    /// @pre state() == UnitState::kCreated
    /// @return kOk; kInvalidArgument if @p dependency is this unit; kAlreadyExists if it was declared
    ///         before; kResourceLimit beyond kMaxUnitDependencies.
    Status depends_on(Unit& dependency) noexcept;

    [[nodiscard]] std::size_t dependency_count() const noexcept {
        return dependency_count_;
    }

    /// @pre index < dependency_count()
    [[nodiscard]] const Unit& dependency(std::size_t index) const noexcept;

    /// kCreated: calls on_initialize(), then kInitialized if it succeeded.
    /// @return kOk; the error of on_initialize(); kInvalidArgument in another state.
    Status initialize() noexcept;

    /// kInitialized: calls on_start(), then kRunning if it succeeded.
    /// @return kOk; the error of on_start(); kAlreadyRunning if running; kInvalidArgument if kCreated.
    Status start() noexcept;

    /// kRunning: calls on_stop(), then kInitialized. Does nothing in another state.
    void stop() noexcept;

    /// Stops the unit if it runs, then, when kInitialized, calls on_shutdown(): kCreated. Does nothing
    /// in kCreated.
    void shutdown() noexcept;

protected:
    /// @pre is_valid_name(name)
    explicit Unit(std::string_view name) noexcept;

    /// @pre state() == UnitState::kCreated: shut the unit down before destroying it.
    ~Unit();

    Unit(const Unit&) = delete;
    Unit(Unit&&) = delete;
    Unit& operator=(const Unit&) = delete;
    Unit& operator=(Unit&&) = delete;

    /// Acquires the unit's resources. Default: nothing, kOk.
    virtual Status on_initialize() noexcept;

    /// Begins operating. Default: nothing, kOk.
    virtual Status on_start() noexcept;

    /// Ends operating; cannot fail. Default: nothing.
    virtual void on_stop() noexcept;

    /// Releases the resources; cannot fail. Default: nothing.
    virtual void on_shutdown() noexcept;

private:
    StaticString<kMaxUnitNameLength> name_{};
    std::atomic<UnitState> state_{UnitState::kCreated};
    std::array<const Unit*, kMaxUnitDependencies> dependencies_{};
    std::size_t dependency_count_{0U};
    bool in_hook_{false};  // A hook is running: its own lifecycle functions must not be called.
};

}  // namespace common

#endif  // COMMON_APP_UNIT_HPP
