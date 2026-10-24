#include "common/core/assert.hpp"

#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace common {
namespace {

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
        buffer[pos] = static_cast<char>(static_cast<std::uint32_t>('0') + (remaining % 10U));
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
