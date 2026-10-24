#include "common/core/assert.hpp"

#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace common {
namespace {

// Digits are written through this table, so the numerical value of a character is never used
// (MISRA C++:2023 Rule 7.0.3).
constexpr std::array<char, 10U> kDigitChars{{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'}};

// write(2) is async-signal-safe and does not allocate, unlike stdio.
void write_stderr(const char* const text) noexcept {
    if (text != nullptr) {
        const ssize_t written{::write(STDERR_FILENO, text, std::strlen(text))};
        static_cast<void>(written);
    }
}

void write_decimal(const std::int32_t value) noexcept {
    std::array<char, 12U> buffer{};
    std::size_t pos{buffer.size() - 1U};  // buffer[pos] stays '\0'.
    std::uint32_t remaining{(value < 0) ? 0U : static_cast<std::uint32_t>(value)};
    do {
        --pos;
        buffer[pos] = kDigitChars[remaining % 10U];
        remaining /= 10U;
    } while ((remaining != 0U) && (pos > 0U));
    write_stderr(&buffer[pos]);
}

void default_handler(const char* const expression, const char* const file, const std::int32_t line) noexcept {
    write_stderr("common: assertion failed: ");
    write_stderr(expression);
    write_stderr(" at ");
    write_stderr(file);
    write_stderr(":");
    write_decimal(line);
    write_stderr("\n");
}

std::atomic<AssertHandler> g_handler{&default_handler};

}  // namespace

void set_assert_handler(const AssertHandler handler) noexcept {
    g_handler.store((handler != nullptr) ? handler : &default_handler, std::memory_order_release);
}

namespace detail {

void assert_fail(const char* const expression, const char* const file, const std::int32_t line) noexcept {
    const AssertHandler handler{g_handler.load(std::memory_order_acquire)};
    handler(expression, file, line);
    std::abort();
}

}  // namespace detail
}  // namespace common
