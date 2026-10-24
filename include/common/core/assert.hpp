/// @file assert.hpp
/// @brief Contract checks that never throw and never allocate.

#ifndef COMMON_CORE_ASSERT_HPP
#define COMMON_CORE_ASSERT_HPP

#include <cstdint>

namespace common {

/// Called when a COMMON_ASSERT fails. The process is aborted after the handler returns.
/// Handlers run on the failing (possibly real-time) thread: they must not allocate or block.
using AssertHandler = void (*)(const char* expression, const char* file, std::int32_t line) noexcept;

/// Installs a custom handler; nullptr restores the default (async-signal-safe write to stderr).
void set_assert_handler(AssertHandler handler) noexcept;

namespace detail {
[[noreturn]] void assert_fail(const char* expression, const char* file, std::int32_t line) noexcept;
}  // namespace detail

}  // namespace common

// Deviation (MISRA C++:2023 Rule 19.0.2): a function-like macro is required to capture the call site
// and the stringified expression. Checks stay enabled in release builds unless explicitly disabled.
#if defined(COMMON_DISABLE_ASSERTS)
#define COMMON_ASSERT(expr) static_cast<void>(sizeof(expr))
#else
#define COMMON_ASSERT(expr) \
    ((expr) ? static_cast<void>(0) : ::common::detail::assert_fail(#expr, __FILE__, __LINE__))
#endif

#endif  // COMMON_CORE_ASSERT_HPP
