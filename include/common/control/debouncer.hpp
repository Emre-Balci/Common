/// @file debouncer.hpp
/// @brief Debouncing of binary inputs by consecutive-sample counting.

#ifndef COMMON_CONTROL_DEBOUNCER_HPP
#define COMMON_CONTROL_DEBOUNCER_HPP

#include <cstdint>

#include "common/core/assert.hpp"
#include "common/core/config.hpp"

namespace common {

/// Accepts a change of a binary input (a switch, a plausibility flag) only after the new value
/// has been seen on a number of consecutive samples, which filters contact bounce and glitches.
class Debouncer final {
public:
    /// @pre samples_to_set > 0, samples_to_clear > 0
    Debouncer(const std::uint32_t samples_to_set, const std::uint32_t samples_to_clear,
              const bool initial_state = false) noexcept
        : samples_to_set_{samples_to_set}, samples_to_clear_{samples_to_clear}, state_{initial_state} {
        COMMON_ASSERT((samples_to_set > 0U) && (samples_to_clear > 0U));
    }

    /// Feeds one raw sample. @return the debounced state.
    bool update(const bool raw) noexcept {
        if (raw == state_) {
            pending_ = 0U;
        } else {
            ++pending_;
            const std::uint32_t required{state_ ? samples_to_clear_ : samples_to_set_};
            if (pending_ >= required) {
                state_ = raw;
                pending_ = 0U;
            }
        }
        return state_;
    }

    void reset(const bool state) noexcept {
        state_ = state;
        pending_ = 0U;
    }

    [[nodiscard]] bool state() const noexcept {
        return state_;
    }

private:
    std::uint32_t samples_to_set_;
    std::uint32_t samples_to_clear_;
    std::uint32_t pending_{0U};  // Consecutive samples that disagree with state_.
    bool state_;
};

}  // namespace common

#endif  // COMMON_CONTROL_DEBOUNCER_HPP
