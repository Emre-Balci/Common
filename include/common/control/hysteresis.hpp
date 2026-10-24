/// @file hysteresis.hpp
/// @brief Two-threshold switch (Schmitt trigger).

#ifndef COMMON_CONTROL_HYSTERESIS_HPP
#define COMMON_CONTROL_HYSTERESIS_HPP

#include <type_traits>

#include "common/core/assert.hpp"
#include "common/core/config.hpp"

namespace common {

/// Two-threshold switch (Schmitt trigger): turns on when the input reaches the upper threshold
/// and off when it falls to the lower one, so noise around a single threshold cannot make the
/// output chatter. Works for integer and floating-point inputs; a NaN input leaves it unchanged.
template <typename T>
class Hysteresis final {
    static_assert(std::is_arithmetic<T>::value, "T must be an arithmetic type");

public:
    /// @pre off_threshold < on_threshold
    Hysteresis(const T off_threshold, const T on_threshold, const bool initial_state = false) noexcept
        : off_threshold_{off_threshold}, on_threshold_{on_threshold}, state_{initial_state} {
        COMMON_ASSERT(off_threshold < on_threshold);
    }

    /// @return the new state: on once input >= on threshold, off once input <= off threshold.
    bool update(const T input) noexcept {
        if (state_) {
            // Not "input > off": every comparison with NaN is false, and NaN must keep the state.
            state_ = !(input <= off_threshold_);
        } else {
            state_ = input >= on_threshold_;
        }
        return state_;
    }

    void reset(const bool state) noexcept {
        state_ = state;
    }

    [[nodiscard]] bool state() const noexcept {
        return state_;
    }

private:
    T off_threshold_;
    T on_threshold_;
    bool state_;
};

}  // namespace common

#endif  // COMMON_CONTROL_HYSTERESIS_HPP
