/// @file unit_manager.hpp
/// @brief Brings the units of an application up in dependency order and down in reverse, with rollback.

#ifndef COMMON_APP_UNIT_MANAGER_HPP
#define COMMON_APP_UNIT_MANAGER_HPP

#include <array>
#include <cstddef>
#include <string_view>

#include "common/app/unit.hpp"
#include "common/containers/span.hpp"
#include "common/core/assert.hpp"
#include "common/core/status.hpp"

namespace common {

namespace detail {
namespace unit_manager {

/// Orders @p units so that each comes after its dependencies; among the units whose dependencies are
/// placed, the earliest in @p units comes first (deterministic). @p ordered and @p placed have room
/// for units.size() entries.
/// @return kOk; kNotFound if a dependency is not in @p units (@p failed: the unit that declared it);
///         kInvalidArgument for a dependency cycle (@p failed: a unit on it or after it).
[[nodiscard]] Status order(Span<Unit* const> units, Span<Unit*> ordered, Span<bool> placed, const Unit*& failed) noexcept;

/// @return kOk if every unit is kCreated, else kInvalidArgument (@p failed: the first other one).
[[nodiscard]] Status check_created(Span<Unit* const> units, const Unit*& failed) noexcept;

/// @return kOk if every unit is kInitialized; else kAlreadyRunning for a running unit, kInvalidArgument
///         for a kCreated one (@p failed: that unit).
[[nodiscard]] Status check_startable(Span<Unit* const> units, const Unit*& failed) noexcept;

/// Initialises the units in order; on a failure shuts down the ones initialised before, in reverse.
[[nodiscard]] Status initialize_all(Span<Unit* const> ordered, const Unit*& failed) noexcept;

/// Starts the units in order; on a failure stops the ones started before, in reverse.
[[nodiscard]] Status start_all(Span<Unit* const> ordered, const Unit*& failed) noexcept;

/// Stops the running units in reverse order.
void stop_all(Span<Unit* const> ordered) noexcept;

/// Stops, then shuts down the initialised units in reverse order.
void shutdown_all(Span<Unit* const> ordered) noexcept;

/// @return the unit named @p name, or nullptr.
[[nodiscard]] Unit* find(Span<Unit* const> units, std::string_view name) noexcept;

}  // namespace unit_manager
}  // namespace detail

/// Brings up to @p MaxUnits units up and down together: initialize() and start() go through the units
/// in dependency order (each after the units it depends_on()), stop() and shutdown() in reverse order.
///
/// - The order is deterministic: among the units whose dependencies are done, the one added first.
/// - validate() checks everything initialize() needs before any hook runs: every dependency added
///   (kNotFound), no cycle (kInvalidArgument), every unit kCreated (kInvalidArgument). initialize() and
///   start() check everything first too, so an error found there runs no hook at all.
/// - If a hook fails, the units the same call had already brought up are taken down again in reverse
///   order (initialize() shuts them down, start() stops them), and the hook's error is returned.
///   failed_unit() names the unit behind every failure: the one whose hook failed, or the one that
///   validate() rejected. Call validate() first to tell wiring errors from hook errors.
/// - A unit that was stopped on its own is restarted with Unit::start().
///
/// Initialisation and shutdown phase, one control thread. The units' hooks must not call the manager
/// (COMMON_ASSERT). Declare the manager after its units: its destructor shuts them down.
///
/// @code
///   static CanGatewayUnit g_can{};
///   static EngineUnit g_engine{};
///   static common::UnitManager<16U> g_units{};
///   static_cast<void>(g_engine.depends_on(g_can));
///   static_cast<void>(g_units.add(g_can));
///   static_cast<void>(g_units.add(g_engine));
///   if (common::is_ok(g_units.initialize()) && common::is_ok(g_units.start())) {
///       ...                                    // operate
///       g_units.stop();                        // engine first, then the CAN gateway
///   } else {
///       log the failure of g_units.failed_unit()->name()
///   }
///   g_units.shutdown();
/// @endcode
template <std::size_t MaxUnits>
class UnitManager final {
    static_assert(MaxUnits > 0U, "MaxUnits must be positive");

public:
    UnitManager() noexcept = default;

    /// Shuts the units down.
    ~UnitManager() {
        shutdown();
    }

    UnitManager(const UnitManager&) = delete;
    UnitManager(UnitManager&&) = delete;
    UnitManager& operator=(const UnitManager&) = delete;
    UnitManager& operator=(UnitManager&&) = delete;

    /// Adds @p unit, which must outlive the manager (or until clear()).
    /// @return kOk; kAlreadyExists for the same unit or a unit of the same name; kResourceLimit when
    ///         MaxUnits units are added; kInvalidArgument after a successful initialize() (until shutdown()).
    Status add(Unit& unit) noexcept {
        COMMON_ASSERT(!busy_);
        Status status{Status::kOk};
        if (up_) {
            status = Status::kInvalidArgument;
        } else if ((contains(unit)) || (detail::unit_manager::find(units(), unit.name()) != nullptr)) {
            status = Status::kAlreadyExists;
        } else if (size_ >= MaxUnits) {
            status = Status::kResourceLimit;
        } else {
            units_[size_] = &unit;
            ++size_;
            ordered_valid_ = false;
        }
        return status;
    }

    /// The checks of initialize(), without calling any hook (see the class); also computes the order.
    /// @return kOk, kNotFound or kInvalidArgument; failed_unit() names the unit at fault.
    [[nodiscard]] Status validate() noexcept {
        COMMON_ASSERT(!busy_);
        failed_ = nullptr;
        Status status{compute_order()};
        if (is_ok(status)) {
            status = detail::unit_manager::check_created(ordered(), failed_);
        }
        return status;
    }

    /// validate(), then initialises the units in dependency order (see the class).
    /// @return kOk, an error of validate(), or the error of the on_initialize() that failed.
    Status initialize() noexcept {
        Status status{validate()};
        if (is_ok(status)) {
            busy_ = true;
            status = detail::unit_manager::initialize_all(ordered(), failed_);
            busy_ = false;
            up_ = is_ok(status);
        }
        return status;
    }

    /// Starts the units in dependency order (see the class).
    /// @return kOk; kNotFound or kInvalidArgument for a wiring error (see validate()); kAlreadyRunning
    ///         if a unit runs; kInvalidArgument if a unit is not initialised; or the error of the
    ///         on_start() that failed.
    Status start() noexcept {
        COMMON_ASSERT(!busy_);
        failed_ = nullptr;
        Status status{ordered_valid_ ? Status::kOk : compute_order()};
        if (is_ok(status)) {
            status = detail::unit_manager::check_startable(ordered(), failed_);
        }
        if (is_ok(status)) {
            busy_ = true;
            status = detail::unit_manager::start_all(ordered(), failed_);
            busy_ = false;
        }
        return status;
    }

    /// Stops the running units in reverse dependency order.
    void stop() noexcept {
        COMMON_ASSERT(!busy_);
        busy_ = true;
        detail::unit_manager::stop_all(lifecycle_order());
        busy_ = false;
    }

    /// Stops the running units, then shuts down every initialised unit, in reverse dependency order.
    void shutdown() noexcept {
        COMMON_ASSERT(!busy_);
        busy_ = true;
        detail::unit_manager::shutdown_all(lifecycle_order());
        busy_ = false;
        up_ = false;
    }

    /// Forgets every unit. @pre every unit is kCreated
    void clear() noexcept {
        COMMON_ASSERT(!busy_);
        const Unit* failed{nullptr};
        COMMON_ASSERT(is_ok(detail::unit_manager::check_created(units(), failed)));
        size_ = 0U;
        ordered_valid_ = false;
        failed_ = nullptr;
        up_ = false;
    }

    /// @return the unit named @p name, or nullptr.
    [[nodiscard]] Unit* find(const std::string_view name) noexcept {
        return detail::unit_manager::find(units(), name);
    }

    /// @return the unit named @p name, or nullptr.
    [[nodiscard]] const Unit* find(const std::string_view name) const noexcept {
        return detail::unit_manager::find(units(), name);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    /// The units in dependency order once it is known (after validate(), initialize() or start()),
    /// otherwise in add() order.
    /// @pre index < size()
    [[nodiscard]] Unit& operator[](const std::size_t index) noexcept {
        COMMON_ASSERT(index < size_);
        return ordered_valid_ ? *ordered_[index] : *units_[index];
    }

    /// See above.
    [[nodiscard]] const Unit& operator[](const std::size_t index) const noexcept {
        COMMON_ASSERT(index < size_);
        return ordered_valid_ ? *ordered_[index] : *units_[index];
    }

    /// The unit at fault in the last validate(), initialize() or start(); nullptr if it succeeded.
    [[nodiscard]] const Unit* failed_unit() const noexcept {
        return failed_;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return MaxUnits;
    }

private:
    [[nodiscard]] Span<Unit* const> units() const noexcept {
        return Span<Unit* const>{units_.data(), size_};
    }

    [[nodiscard]] Span<Unit* const> ordered() const noexcept {
        return Span<Unit* const>{ordered_.data(), size_};
    }

    [[nodiscard]] Span<Unit* const> lifecycle_order() const noexcept {
        return ordered_valid_ ? ordered() : units();
    }

    [[nodiscard]] bool contains(const Unit& unit) const noexcept {
        bool found{false};
        for (std::size_t index{0U}; (index < size_) && (!found); ++index) {
            found = (units_[index] == &unit);
        }
        return found;
    }

    [[nodiscard]] Status compute_order() noexcept {
        const Status status{detail::unit_manager::order(units(), Span<Unit*>{ordered_.data(), size_},
                                                        Span<bool>{placed_.data(), size_}, failed_)};
        ordered_valid_ = is_ok(status);
        return status;
    }

    std::array<Unit*, MaxUnits> units_{};    // In add() order.
    std::array<Unit*, MaxUnits> ordered_{};  // In dependency order, when ordered_valid_.
    std::array<bool, MaxUnits> placed_{};    // Scratch of order().
    std::size_t size_{0U};
    const Unit* failed_{nullptr};
    bool ordered_valid_{false};
    bool up_{false};    // Initialised by initialize(), until shutdown().
    bool busy_{false};  // A lifecycle call is running the units' hooks.
};

}  // namespace common

#endif  // COMMON_APP_UNIT_MANAGER_HPP
