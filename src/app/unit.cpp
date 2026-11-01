#include "common/app/unit.hpp"

#include <atomic>
#include <cstddef>

#include "common/core/assert.hpp"

namespace common {

namespace {

static_assert(std::atomic<UnitState>::is_always_lock_free, "std::atomic<UnitState> must be lock-free");

}  // namespace

Unit::Unit(const std::string_view name) noexcept {
    COMMON_ASSERT(is_valid_name(name));
    static_cast<void>(name_.assign(name));
}

Unit::~Unit() {
    COMMON_ASSERT(state() == UnitState::kCreated);
}

Status Unit::depends_on(Unit& dependency) noexcept {
    COMMON_ASSERT(state() == UnitState::kCreated);
    Status status{Status::kOk};
    bool declared{false};
    for (std::size_t index{0U}; index < dependency_count_; ++index) {
        declared = declared || (dependencies_[index] == &dependency);
    }
    if (&dependency == this) {
        status = Status::kInvalidArgument;
    } else if (declared) {
        status = Status::kAlreadyExists;
    } else if (dependency_count_ >= kMaxUnitDependencies) {
        status = Status::kResourceLimit;
    } else {
        dependencies_[dependency_count_] = &dependency;
        ++dependency_count_;
    }
    return status;
}

const Unit& Unit::dependency(const std::size_t index) const noexcept {
    COMMON_ASSERT(index < dependency_count_);
    return *dependencies_[index];
}

Status Unit::initialize() noexcept {
    COMMON_ASSERT(!in_hook_);
    Status status{Status::kInvalidArgument};
    if (state() == UnitState::kCreated) {
        in_hook_ = true;
        status = on_initialize();
        in_hook_ = false;
        if (is_ok(status)) {
            state_.store(UnitState::kInitialized, std::memory_order_release);
        }
    }
    return status;
}

Status Unit::start() noexcept {
    COMMON_ASSERT(!in_hook_);
    Status status{Status::kInvalidArgument};
    const UnitState current{state()};
    if (current == UnitState::kRunning) {
        status = Status::kAlreadyRunning;
    } else if (current == UnitState::kInitialized) {
        in_hook_ = true;
        status = on_start();
        in_hook_ = false;
        if (is_ok(status)) {
            state_.store(UnitState::kRunning, std::memory_order_release);
        }
    } else {
        // kCreated: not initialised, kInvalidArgument.
    }
    return status;
}

void Unit::stop() noexcept {
    COMMON_ASSERT(!in_hook_);
    if (state() == UnitState::kRunning) {
        in_hook_ = true;
        on_stop();
        in_hook_ = false;
        state_.store(UnitState::kInitialized, std::memory_order_release);
    }
}

void Unit::shutdown() noexcept {
    COMMON_ASSERT(!in_hook_);
    stop();
    if (state() == UnitState::kInitialized) {
        in_hook_ = true;
        on_shutdown();
        in_hook_ = false;
        state_.store(UnitState::kCreated, std::memory_order_release);
    }
}

Status Unit::on_initialize() noexcept {
    return Status::kOk;
}

Status Unit::on_start() noexcept {
    return Status::kOk;
}

void Unit::on_stop() noexcept {}

void Unit::on_shutdown() noexcept {}

}  // namespace common
