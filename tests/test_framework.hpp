/// @file test_framework.hpp
/// @brief Minimal dependency-free test harness (works with -fno-exceptions).

#ifndef COMMON_TEST_FRAMEWORK_HPP
#define COMMON_TEST_FRAMEWORK_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace test {

using TestFunction = void (*)();

struct TestCase final {
    const char* name;
    TestFunction function;
};

class Registry final {
public:
    static constexpr std::size_t kMaxTests{512U};

    static Registry& instance() noexcept;
    void add(const char* name, TestFunction function) noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] const TestCase& at(const std::size_t index) const noexcept {
        return cases_[index];
    }

private:
    std::array<TestCase, kMaxTests> cases_{};
    std::size_t count_{0U};
};

struct Registrar final {
    Registrar(const char* name, TestFunction function) noexcept {
        Registry::instance().add(name, function);
    }
};

/// Thread-safe: checks may run on worker threads.
void report_failure(const char* file, std::int32_t line, const char* expression) noexcept;
std::uint32_t failure_count() noexcept;

}  // namespace test

#define TEST_CASE(name)                                                  \
    static void name();                                                  \
    static const ::test::Registrar name##_registrar{#name, &name};       \
    static void name()

#define CHECK(expr)                                                      \
    do {                                                                 \
        if (!(expr)) {                                                   \
            ::test::report_failure(__FILE__, __LINE__, #expr);           \
        }                                                                \
    } while (false)

#define REQUIRE(expr)                                                    \
    do {                                                                 \
        if (!(expr)) {                                                   \
            ::test::report_failure(__FILE__, __LINE__, #expr);           \
            return;                                                      \
        }                                                                \
    } while (false)

#endif  // COMMON_TEST_FRAMEWORK_HPP
