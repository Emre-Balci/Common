#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <crtdbg.h>

#include "test_platform.hpp"
#endif

#include "test_framework.hpp"

namespace test {
namespace {
std::atomic<std::uint32_t> g_failures{0U};
}  // namespace

Registry& Registry::instance() noexcept {
    static Registry registry{};
    return registry;
}

void Registry::add(const char* const name, const TestFunction function) noexcept {
    if (count_ >= kMaxTests) {
        std::fprintf(stderr, "too many tests, raise Registry::kMaxTests\n");
        std::abort();
    }
    cases_[count_] = TestCase{name, function};
    ++count_;
}

void report_failure(const char* const file, const std::int32_t line, const char* const expression) noexcept {
    g_failures.fetch_add(1U, std::memory_order_relaxed);
    std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, expression);
}

std::uint32_t failure_count() noexcept {
    return g_failures.load(std::memory_order_relaxed);
}

}  // namespace test

/// Usage: common_base_tests [substring] or common_tests [substring]: runs the tests whose name
/// contains substring.
int main(const int argc, const char* const argv[]) {
#if defined(_WIN32)
    // A failure must end the run, not wait for someone to close a dialog: the debug C runtime reports
    // its assertions and errors on stderr, and abort() and crashes end the process at once.
    static_cast<void>(_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE));
    static_cast<void>(_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR));
    static_cast<void>(_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE));
    static_cast<void>(_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR));
    static_cast<void>(_set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT));
    static_cast<void>(::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX));
#endif
    const char* const filter{(argc > 1) ? argv[1] : nullptr};
    const test::Registry& registry{test::Registry::instance()};
    std::uint32_t run{0U};
    std::uint32_t failed{0U};
    for (std::size_t i{0U}; i < registry.size(); ++i) {
        const test::TestCase& test_case{registry.at(i)};
        if ((filter != nullptr) && (std::strstr(test_case.name, filter) == nullptr)) {
            continue;
        }
        const std::uint32_t before{test::failure_count()};
        std::printf("[ RUN  ] %s\n", test_case.name);
        std::fflush(stdout);
        test_case.function();
        const bool ok{test::failure_count() == before};
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", test_case.name);
        ++run;
        failed += ok ? 0U : 1U;
    }
    std::printf("\n%u test(s) run, %u failed\n", run, failed);
    return (failed == 0U) ? EXIT_SUCCESS : EXIT_FAILURE;
}
