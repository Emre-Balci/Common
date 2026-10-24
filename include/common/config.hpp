/// @file config.hpp
/// @brief Platform checks and library-wide constants.

#ifndef COMMON_CONFIG_HPP
#define COMMON_CONFIG_HPP

#include <cstddef>
#include <type_traits>

#if !defined(__linux__)
#error "common: only Linux targets are supported"
#endif

#if (__cplusplus < 201703L)
#error "common: C++17 or later is required"
#endif

// -ffast-math silently breaks IEEE-754 semantics (NaN/Inf handling, associativity, FTZ at startup).
#if defined(__FAST_MATH__)
#error "common: -ffast-math is not allowed"
#endif

namespace common {

/// Size of a destructive-interference unit on the supported targets (x86-64, AArch64).
/// std::hardware_destructive_interference_size is avoided because its value is not ABI-stable in GCC.
inline constexpr std::size_t kCacheLineSize{64U};

/// Smallest page size on the supported targets; used to stride when pre-faulting memory.
inline constexpr std::size_t kPageSize{4096U};

/// @return true when @p value is a non-zero power of two.
template <typename T>
constexpr bool is_power_of_two(const T value) noexcept {
    static_assert(std::is_unsigned<T>::value, "is_power_of_two requires an unsigned type");
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

}  // namespace common

#endif  // COMMON_CONFIG_HPP
