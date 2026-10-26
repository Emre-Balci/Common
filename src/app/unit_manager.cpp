#include "common/app/unit_manager.hpp"

#include <cstddef>
#include <string_view>

namespace common {
namespace detail {
namespace unit_manager {

namespace {

/// @return the index of @p unit in @p units, or units.size().
[[nodiscard]] std::size_t index_of(const Span<Unit* const> units, const Unit* const unit) noexcept {
    std::size_t found{units.size()};
    for (std::size_t index{0U}; (index < units.size()) && (found == units.size()); ++index) {
        if (units[index] == unit) {
            found = index;
        }
    }
    return found;
}

/// @return true if every dependency of @p unit is placed already.
[[nodiscard]] bool dependencies_placed(const Span<Unit* const> units, const Span<bool> placed, const Unit& unit) noexcept {
    bool all{true};
    for (std::size_t d{0U}; (d < unit.dependency_count()) && all; ++d) {
        const std::size_t index{index_of(units, &unit.dependency(d))};
        all = (index < units.size()) && placed[index];
    }
    return all;
}

/// Compares a unit name with @p text, character by character.
[[nodiscard]] bool name_equals(const char* const name, const std::string_view text) noexcept {
    bool equal{text.size() <= kMaxUnitNameLength};
    for (std::size_t i{0U}; equal && (i < text.size()); ++i) {
        equal = (name[i] == text[i]);
    }
    return equal && (name[text.size()] == '\0');
}

}  // namespace

Status order(const Span<Unit* const> units, const Span<Unit*> ordered, const Span<bool> placed,
             const Unit*& failed) noexcept {
    Status status{Status::kOk};
    const std::size_t count{units.size()};
    for (std::size_t u{0U}; (u < count) && is_ok(status); ++u) {
        const Unit& unit{*units[u]};
        for (std::size_t d{0U}; (d < unit.dependency_count()) && is_ok(status); ++d) {
            if (index_of(units, &unit.dependency(d)) == count) {
                failed = &unit;
                status = Status::kNotFound;
            }
        }
    }
    for (std::size_t index{0U}; index < count; ++index) {
        placed[index] = false;
    }
    for (std::size_t position{0U}; (position < count) && is_ok(status); ++position) {
        std::size_t chosen{count};
        for (std::size_t index{0U}; (index < count) && (chosen == count); ++index) {
            if ((!placed[index]) && dependencies_placed(units, placed, *units[index])) {
                chosen = index;
            }
        }
        if (chosen < count) {
            placed[chosen] = true;
            ordered[position] = units[chosen];
        } else {
            // Every unit left waits for another: a cycle. Name the first of them.
            for (std::size_t index{0U}; (index < count) && is_ok(status); ++index) {
                if (!placed[index]) {
                    failed = units[index];
                    status = Status::kInvalidArgument;
                }
            }
        }
    }
    return status;
}

Status check_created(const Span<Unit* const> units, const Unit*& failed) noexcept {
    Status status{Status::kOk};
    for (std::size_t index{0U}; (index < units.size()) && is_ok(status); ++index) {
        if (units[index]->state() != UnitState::kCreated) {
            failed = units[index];
            status = Status::kInvalidArgument;
        }
    }
    return status;
}

Status check_startable(const Span<Unit* const> units, const Unit*& failed) noexcept {
    Status status{Status::kOk};
    for (std::size_t index{0U}; (index < units.size()) && is_ok(status); ++index) {
        const UnitState state{units[index]->state()};
        if (state == UnitState::kRunning) {
            failed = units[index];
            status = Status::kAlreadyRunning;
        } else if (state == UnitState::kCreated) {
            failed = units[index];
            status = Status::kInvalidArgument;
        } else {
            // kInitialized: startable.
        }
    }
    return status;
}

Status initialize_all(const Span<Unit* const> ordered, const Unit*& failed) noexcept {
    Status status{Status::kOk};
    std::size_t done{0U};
    for (std::size_t index{0U}; (index < ordered.size()) && is_ok(status); ++index) {
        status = ordered[index]->initialize();
        if (is_ok(status)) {
            ++done;
        } else {
            failed = ordered[index];
        }
    }
    if (!is_ok(status)) {
        for (std::size_t index{done}; index > 0U; --index) {
            ordered[index - 1U]->shutdown();
        }
    }
    return status;
}

Status start_all(const Span<Unit* const> ordered, const Unit*& failed) noexcept {
    Status status{Status::kOk};
    std::size_t done{0U};
    for (std::size_t index{0U}; (index < ordered.size()) && is_ok(status); ++index) {
        status = ordered[index]->start();
        if (is_ok(status)) {
            ++done;
        } else {
            failed = ordered[index];
        }
    }
    if (!is_ok(status)) {
        for (std::size_t index{done}; index > 0U; --index) {
            ordered[index - 1U]->stop();
        }
    }
    return status;
}

void stop_all(const Span<Unit* const> ordered) noexcept {
    for (std::size_t index{ordered.size()}; index > 0U; --index) {
        ordered[index - 1U]->stop();
    }
}

void shutdown_all(const Span<Unit* const> ordered) noexcept {
    stop_all(ordered);
    for (std::size_t index{ordered.size()}; index > 0U; --index) {
        ordered[index - 1U]->shutdown();
    }
}

Unit* find(const Span<Unit* const> units, const std::string_view name) noexcept {
    Unit* found{nullptr};
    for (std::size_t index{0U}; (index < units.size()) && (found == nullptr); ++index) {
        if (name_equals(units[index]->name(), name)) {
            found = units[index];
        }
    }
    return found;
}

}  // namespace unit_manager
}  // namespace detail
}  // namespace common
