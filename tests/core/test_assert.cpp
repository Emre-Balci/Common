#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>

#include "common/core/assert.hpp"
#include "test_framework.hpp"

namespace {

constexpr std::int32_t kAssertLine{__LINE__ + 3};  // The line of the COMMON_ASSERT in fail_assertion().

void fail_assertion(const bool condition) noexcept {
    COMMON_ASSERT(condition);
}

void custom_handler(const char* const expression, const char* const /*file*/, const std::int32_t line) noexcept {
    const std::string text{std::string{"custom: "} + expression + " " + std::to_string(line) + "\n"};
    static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()) >= 0);
}

struct ChildResult final {
    std::string output;   // What the child wrote to stderr.
    bool aborted{false};  // It ended with SIGABRT.
};

/// Runs fail_assertion(false) in a child process with @p handler installed and its stderr
/// connected to a pipe.
ChildResult fail_in_child(const common::AssertHandler handler) {
    ChildResult result{};
    std::array<int, 2U> pipe_ends{{-1, -1}};
    if (::pipe(pipe_ends.data()) != 0) {
        return result;
    }
    static_cast<void>(std::fflush(nullptr));  // The child must not repeat buffered output.
    const pid_t child{::fork()};
    if (child == 0) {
        static_cast<void>(::dup2(pipe_ends[1U], STDERR_FILENO));
        common::set_assert_handler(handler);
        fail_assertion(false);
        ::_exit(0);  // Not reached: a failed assertion aborts.
    }
    static_cast<void>(::close(pipe_ends[1U]));
    std::array<char, 512U> buffer{};
    for (std::uint32_t round{0U}; round < 100U; ++round) {
        const ssize_t count{::read(pipe_ends[0U], buffer.data(), buffer.size())};
        if (count <= 0) {
            break;
        }
        result.output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    static_cast<void>(::close(pipe_ends[0U]));
    int status{0};
    if ((child > 0) && (::waitpid(child, &status, 0) == child)) {
        result.aborted = WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT);
    }
    return result;
}

}  // namespace

TEST_CASE(assert_default_handler_reports_the_call_site) {
    const ChildResult result{fail_in_child(nullptr)};  // nullptr installs the default handler.
    CHECK(result.aborted);
    CHECK(result.output ==
          std::string{"common: assertion failed: condition at "} + __FILE__ + ":" + std::to_string(kAssertLine) + "\n");
}

TEST_CASE(assert_custom_handler_runs_before_abort) {
    const ChildResult result{fail_in_child(&custom_handler)};
    CHECK(result.aborted);
    CHECK(result.output == "custom: condition " + std::to_string(kAssertLine) + "\n");
}
