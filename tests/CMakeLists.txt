# Test sources mirror the header groups in include/common/<group>/.
add_executable(common_tests
    test_main.cpp
    concurrency/test_queues.cpp
    concurrency/test_seqlock.cpp
    concurrency/test_triple_buffer.cpp
    containers/test_containers.cpp
    containers/test_span.cpp
    control/test_control.cpp
    core/test_status.cpp
    diagnostics/test_latency_histogram.cpp
    diagnostics/test_watchdog.cpp
    file/test_file.cpp
    numeric/test_float.cpp
    numeric/test_integer.cpp
    protocol/test_rate_limiter.cpp
    system/test_thread.cpp
    time/test_clock.cpp)

# test_framework.hpp lives here; tests in group subdirectories include it by name.
target_include_directories(common_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(common_tests PRIVATE common::common common::options)

add_test(NAME common_tests COMMAND common_tests)
