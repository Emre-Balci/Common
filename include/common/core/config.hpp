/// @file config.hpp
/// @brief Platform checks and library-wide constants.

#ifndef COMMON_CORE_CONFIG_HPP
#define COMMON_CORE_CONFIG_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#if !defined(__linux__) && !defined(_WIN32) && !defined(__APPLE__)
#error "common: only Linux (including Android), Windows, macOS and iOS targets are supported"
#endif

// COMMON_LINUX_API is 1 where the calls of Linux beyond POSIX are there (ppoll, clock_nanosleep,
// MAP_POPULATE): Linux and Android. It is 0 on macOS and iOS, which use portable POSIX code in their
// place, and on Linux built with COMMON_PORTABLE_POSIX, so that the tests cover that code there too.
// Windows has code of its own.
#if defined(__linux__) && !defined(COMMON_PORTABLE_POSIX)
#define COMMON_LINUX_API 1
#else
#define COMMON_LINUX_API 0
#endif

// Android 9 brought pthread_attr_setinheritsched, without which RtThread could not keep a thread
// from inheriting the policy of its creator.
#if defined(__ANDROID__)
#include <android/api-level.h>
#if (__ANDROID_API__ < 28)
#error "common: Android API level 28 (Android 9) or later is required"
#endif
#endif

// MSVC reports __cplusplus as 199711L unless /Zc:__cplusplus is given; _MSVC_LANG always holds the
// language version.
#if defined(_MSVC_LANG)
#if (_MSVC_LANG < 201703L)
#error "common: C++17 or later is required"
#endif
#elif (__cplusplus < 201703L)
#error "common: C++17 or later is required"
#endif

// -ffast-math (GCC, Clang) and /fp:fast (MSVC) silently break IEEE-754 semantics (NaN/Inf handling,
// associativity, FTZ at startup).
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "common: -ffast-math and /fp:fast are not allowed"
#endif

namespace common {

/// Size of a destructive-interference unit on the supported targets (x86-64, AArch64).
/// std::hardware_destructive_interference_size is avoided because its value is not ABI-stable in GCC.
inline constexpr std::size_t kCacheLineSize{64U};

/// Smallest page size on the supported targets; used to stride when pre-faulting memory.
inline constexpr std::size_t kPageSize{4096U};

/// The operating system's handle of an open file or device: a file descriptor on Linux, a HANDLE
/// on Windows.
#if defined(_WIN32)
using NativeHandle = void*;
/// What a closed File or SerialPort reports as its handle.
inline constexpr NativeHandle kInvalidNativeHandle{nullptr};
#else
using NativeHandle = std::int32_t;
/// What a closed File or SerialPort reports as its descriptor.
inline constexpr NativeHandle kInvalidNativeHandle{-1};
#endif

/// @return true when @p value is a non-zero power of two.
template <typename T>
constexpr bool is_power_of_two(const T value) noexcept {
    static_assert(std::is_unsigned<T>::value, "is_power_of_two requires an unsigned type");
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

}  // namespace common

#endif  // COMMON_CORE_CONFIG_HPP
