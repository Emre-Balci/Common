#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

#include "common/core/assert.hpp"
#include "test_framework.hpp"
#if defined(_WIN32)
#include "test_platform.hpp"
#endif

namespace {

constexpr std::int32_t kAssertLine{__LINE__ + 3};  // The line of the COMMON_ASSERT in fail_assertion().

void fail_assertion(const bool condition) noexcept {
    COMMON_ASSERT(condition);
}

void custom_handler(const char* const expression, const char* const /*file*/, const std::int32_t line) noexcept {
    const std::string text{std::string{"custom: "} + expression + " " + std::to_string(line) + "\n"};
#if defined(_WIN32)
    DWORD written{0U};
    static_cast<void>(::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), text.data(), static_cast<DWORD>(text.size()),
                                  &written, nullptr));
#else
    static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()) >= 0);
#endif
}

struct ChildResult final {
    std::string output;   // What the child wrote to stderr.
    bool aborted{false};  // It ended with SIGABRT.
};

#if defined(_WIN32)

// Windows has no fork(): the test runs this program again with this option, and the child fails the
// assertion while it initialises, before main() runs any test.
constexpr const wchar_t* kChildOption{L"--assert-child="};

/// In the child: installs the handler that the command line names and fails the assertion.
/// @return false in the parent.
bool fail_if_child() noexcept {
    const wchar_t* const option{std::wcsstr(::GetCommandLineW(), kChildOption)};
    if (option != nullptr) {
        // abort() must neither show a dialog nor start Windows Error Reporting: the parent only wants
        // the exit code.
        static_cast<void>(_set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT));
        static_cast<void>(::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX));
        const bool custom{std::wcsncmp(option + std::wcslen(kChildOption), L"custom", 6U) == 0};
        common::set_assert_handler(custom ? &custom_handler : nullptr);
        fail_assertion(false);
    }
    return option != nullptr;
}

[[maybe_unused]] const bool g_child{fail_if_child()};

/// Runs fail_assertion(false) in a child process with @p handler installed and its stderr
/// connected to a pipe.
ChildResult fail_in_child(const common::AssertHandler handler) {
    ChildResult result{};
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE read_end{nullptr};
    HANDLE write_end{nullptr};
    std::array<wchar_t, 32768U> program{};
    if ((::CreatePipe(&read_end, &write_end, &inheritable, 0U) == FALSE) ||
        (::GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size())) == 0U)) {
        return result;
    }
    static_cast<void>(::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0U));  // Only the write end.
    std::wstring command{L"\"" + std::wstring{program.data()} + L"\" " + kChildOption +
                         ((handler == nullptr) ? L"default" : L"custom")};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdError = write_end;
    PROCESS_INFORMATION child{};
    const BOOL created{::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0U, nullptr, nullptr,
                                        &startup, &child)};
    static_cast<void>(::CloseHandle(write_end));  // The child holds the only write end now.
    if (created != FALSE) {
        std::array<char, 512U> buffer{};
        for (std::uint32_t round{0U}; round < 100U; ++round) {
            const std::int64_t count{test::read_native(read_end, buffer.data(), buffer.size())};
            if (count <= 0) {
                break;
            }
            result.output.append(buffer.data(), static_cast<std::size_t>(count));
        }
        DWORD code{0U};
        if ((::WaitForSingleObject(child.hProcess, 10000U) == WAIT_OBJECT_0) &&
            (::GetExitCodeProcess(child.hProcess, &code) != FALSE)) {
            result.aborted = (code == 3U);  // abort() ends the process with exit code 3.
        }
        static_cast<void>(::CloseHandle(child.hThread));
        static_cast<void>(::CloseHandle(child.hProcess));
    }
    static_cast<void>(::CloseHandle(read_end));
    return result;
}

#else

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

#endif

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
